/* language: C++17, file: android/hwcomposer/hwcomposer_x11.cpp
 * Minimal HWC2 for x11-droid.
 *
 * Strategy: every layer is forced to HWC2_COMPOSITION_CLIENT. SurfaceFlinger
 * composes into the client target, then presentDisplay copies RGBA/BGRA pixels
 * to /dev/shm/x11droid_fb and signals the host daemon.
 */
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <hardware/hwcomposer.h>
#include <hardware/hwcomposer2.h>

#include "xd_fb_layout.h"
#include "xd_gralloc.h"
#include "xd_log.h"
#include "xd_shm.h"

namespace {

using xd::FbHeader;
using xd::GrallocMapper;
using xd::MappedBuffer;
using xd::ShmSink;

constexpr hwc2_display_t kPrimaryDisplay = 0;
constexpr hwc2_config_t kActiveConfig = 1;
constexpr uint32_t kVsyncPeriodNs = 16666667u; /* 59.94 Hz */
constexpr int32_t kDpi = 320;
constexpr int kFenceTimeoutMs = 250;

struct LayerState {
    hwc2_layer_t id = 0;
    hwc2_composition_t composition = HWC2_COMPOSITION_INVALID;
    hwc_rect_t display_frame {};
    hwc_frect_t source_crop {};
    hwc2_blend_mode_t blend_mode = HWC2_BLEND_MODE_INVALID;
    int32_t transform = 0;
    hwc_color_t color {};
    float plane_alpha = 1.0f;
    uint32_t z_order = 0;
};

struct DisplayState {
    hwc2_power_mode_t power = HWC2_POWER_MODE_OFF;
    hwc2_config_t active_config = kActiveConfig;
    bool validated = false;
    bool vsync_enabled = false;

    buffer_handle_t client_target = nullptr;
    int32_t client_acquire_fence = -1;

    std::vector<LayerState> layers;
    std::vector<hwc2_layer_t> changed_layers;
    std::vector<hwc2_composition_t> changed_types;

    uint64_t frames = 0;
    uint64_t map_failures = 0;
    uint64_t fence_timeouts = 0;
};

struct CallbackSlot {
    hwc2_callback_data_t data = nullptr;
    hwc2_function_pointer_t fn = nullptr;
};

std::mutex g_lock;
DisplayState g_display;
hwc2_layer_t g_next_layer = 1;

std::mutex g_cb_lock;
CallbackSlot g_cb_hotplug;
CallbackSlot g_cb_refresh;
CallbackSlot g_cb_vsync;
CallbackSlot g_cb_vsync24;

std::condition_variable g_vsync_cv;
std::thread* g_vsync_thread = nullptr;
bool g_vsync_stop = false;

ShmSink g_shm;
GrallocMapper g_gralloc;
hwc2_device_t g_device {};

void reset_state_locked() {
    g_display = DisplayState {};
    g_display.active_config = kActiveConfig;
    g_next_layer = 1;
}

LayerState* find_layer_locked(hwc2_layer_t id) {
    for (auto& layer : g_display.layers) {
        if (layer.id == id) {
            return &layer;
        }
    }
    return nullptr;
}

void close_fence(int32_t& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void wait_fence(int fd, uint64_t* timeouts) {
    if (fd < 0) {
        return;
    }
    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLIN;

    int rc;
    do {
        rc = poll(&pfd, 1, kFenceTimeoutMs);
    } while (rc < 0 && errno == EINTR);

    if (rc == 0) {
        ++(*timeouts);
        XD_LOGW("acquire fence timeout %d ms", kFenceTimeoutMs);
    }
    close(fd);
}

void fire_hotplug() {
    hwc2_callback_data_t data = nullptr;
    hwc2_function_pointer_t fn = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_cb_lock);
        data = g_cb_hotplug.data;
        fn = g_cb_hotplug.fn;
    }
    if (fn != nullptr) {
        reinterpret_cast<HWC2_PFN_HOTPLUG>(fn)(
            data, kPrimaryDisplay, HWC2_CONNECTION_CONNECTED);
    }
}

void vsync_thread_main() {
    std::unique_lock<std::mutex> lk(g_lock);
    while (!g_vsync_stop) {
        if (!g_display.vsync_enabled) {
            g_vsync_cv.wait(lk);
            continue;
        }
        lk.unlock();

        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const int64_t now_ns =
            static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;

        hwc2_callback_data_t data24 = nullptr;
        hwc2_function_pointer_t fn24 = nullptr;
        hwc2_callback_data_t data = nullptr;
        hwc2_function_pointer_t fn = nullptr;
        {
            std::lock_guard<std::mutex> cb_lk(g_cb_lock);
            data24 = g_cb_vsync24.data;
            fn24 = g_cb_vsync24.fn;
            data = g_cb_vsync.data;
            fn = g_cb_vsync.fn;
        }

        if (fn24 != nullptr) {
            reinterpret_cast<HWC2_PFN_VSYNC_2_4>(fn24)(
                data24, kPrimaryDisplay, now_ns, kVsyncPeriodNs);
        } else if (fn != nullptr) {
            reinterpret_cast<HWC2_PFN_VSYNC>(fn)(
                data, kPrimaryDisplay, now_ns);
        }

        lk.lock();
        g_vsync_cv.wait_for(lk, std::chrono::milliseconds(16));
    }
}

int32_t display_error(hwc2_display_t display) {
    return display == kPrimaryDisplay ? HWC2_ERROR_NONE : HWC2_ERROR_BAD_DISPLAY;
}

/* ---------------------------------------------------------------- callbacks */

uint32_t hwc_get_max_virtual_display_count(hwc2_device_t*) {
    return 0;
}

int32_t hwc_create_virtual_display(hwc2_device_t*, uint32_t, uint32_t,
                                   int32_t*, hwc2_display_t*) {
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t hwc_destroy_virtual_display(hwc2_device_t*, hwc2_display_t) {
    return HWC2_ERROR_BAD_DISPLAY;
}

void hwc_dump(hwc2_device_t*, uint32_t* outSize, char* outBuffer) {
    static std::string dump;
    if (outSize == nullptr) {
        return;
    }

    if (outBuffer == nullptr) {
        char line[512];
        std::lock_guard<std::mutex> lk(g_lock);
        const int n = snprintf(
            line, sizeof line,
            "x11droid-hwc: display=%u config=%u %ux%u power=%d layers=%zu "
            "validated=%d frames=%llu map_failures=%llu fence_timeouts=%llu "
            "shm_frames=%llu shm_dropped=%llu\n",
            static_cast<unsigned>(kPrimaryDisplay),
            static_cast<unsigned>(g_display.active_config),
            xd::kDisplayWidth, xd::kDisplayHeight,
            static_cast<int>(g_display.power), g_display.layers.size(),
            g_display.validated ? 1 : 0,
            static_cast<unsigned long long>(g_display.frames),
            static_cast<unsigned long long>(g_display.map_failures),
            static_cast<unsigned long long>(g_display.fence_timeouts),
            static_cast<unsigned long long>(g_shm.frames_presented()),
            static_cast<unsigned long long>(g_shm.frames_dropped()));
        dump.assign(line, n > 0 ? static_cast<size_t>(n) : 0u);
        *outSize = static_cast<uint32_t>(dump.size());
        return;
    }

    std::lock_guard<std::mutex> lk(g_lock);
    const size_t n = dump.size() < *outSize ? dump.size() : *outSize;
    memcpy(outBuffer, dump.data(), n);
    *outSize = static_cast<uint32_t>(n);
}

int32_t hwc_register_callback(hwc2_device_t*, int32_t descriptor,
                              hwc2_callback_data_t data,
                              hwc2_function_pointer_t pointer) {
    CallbackSlot* slot = nullptr;
    switch (descriptor) {
        case HWC2_CALLBACK_HOTPLUG: slot = &g_cb_hotplug; break;
        case HWC2_CALLBACK_REFRESH: slot = &g_cb_refresh; break;
        case HWC2_CALLBACK_VSYNC: slot = &g_cb_vsync; break;
        case HWC2_CALLBACK_VSYNC_2_4: slot = &g_cb_vsync24; break;
        default: return HWC2_ERROR_BAD_PARAMETER;
    }

    {
        std::lock_guard<std::mutex> lk(g_cb_lock);
        slot->data = data;
        slot->fn = pointer;
    }

    /* HWC2 requires at least one hotplug notification, including the built-in display. */
    if (descriptor == HWC2_CALLBACK_HOTPLUG && pointer != nullptr) {
        fire_hotplug();
    }
    return HWC2_ERROR_NONE;
}

/* ---------------------------------------------------------------- display */

int32_t hwc_accept_display_changes(hwc2_device_t*, hwc2_display_t display) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    for (size_t i = 0; i < g_display.changed_layers.size(); ++i) {
        LayerState* layer = find_layer_locked(g_display.changed_layers[i]);
        if (layer != nullptr) {
            layer->composition = g_display.changed_types[i];
        }
    }
    g_display.changed_layers.clear();
    g_display.changed_types.clear();
    return HWC2_ERROR_NONE;
}

int32_t hwc_create_layer(hwc2_device_t*, hwc2_display_t display,
                         hwc2_layer_t* outLayer) {
    if (outLayer == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    LayerState layer;
    layer.id = g_next_layer++;
    g_display.layers.push_back(layer);
    *outLayer = layer.id;
    return HWC2_ERROR_NONE;
}

int32_t hwc_destroy_layer(hwc2_device_t*, hwc2_display_t display,
                          hwc2_layer_t layer) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    for (auto it = g_display.layers.begin(); it != g_display.layers.end(); ++it) {
        if (it->id == layer) {
            g_display.layers.erase(it);
            g_display.changed_layers.clear();
            g_display.changed_types.clear();
            return HWC2_ERROR_NONE;
        }
    }
    return HWC2_ERROR_BAD_LAYER;
}

int32_t hwc_get_active_config(hwc2_device_t*, hwc2_display_t display,
                              hwc2_config_t* outConfig) {
    if (outConfig == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    *outConfig = g_display.active_config;
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_changed_composition_types(hwc2_device_t*, hwc2_display_t display,
                                          uint32_t* outNumElements,
                                          hwc2_layer_t* outLayers,
                                          int32_t* outTypes) {
    if (outNumElements == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    if (!g_display.validated) {
        return HWC2_ERROR_NOT_VALIDATED;
    }

    const size_t count = g_display.changed_layers.size();
    if (outLayers == nullptr || outTypes == nullptr) {
        *outNumElements = static_cast<uint32_t>(count);
        return HWC2_ERROR_NONE;
    }

    const size_t n = count < *outNumElements ? count : *outNumElements;
    for (size_t i = 0; i < n; ++i) {
        outLayers[i] = g_display.changed_layers[i];
        outTypes[i] = static_cast<int32_t>(g_display.changed_types[i]);
    }
    *outNumElements = static_cast<uint32_t>(n);
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_client_target_support(hwc2_device_t*, hwc2_display_t display,
                                      uint32_t width, uint32_t height,
                                      int32_t format, int32_t) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    if (width == xd::kDisplayWidth && height == xd::kDisplayHeight &&
        format == HAL_PIXEL_FORMAT_RGBA_8888) {
        return HWC2_ERROR_NONE;
    }
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t hwc_get_color_modes(hwc2_device_t*, hwc2_display_t display,
                            uint32_t* outNumModes, int32_t* outModes) {
    if (outNumModes == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    if (outModes == nullptr) {
        *outNumModes = 1;
        return HWC2_ERROR_NONE;
    }
    if (*outNumModes > 0) {
        outModes[0] = HAL_COLOR_MODE_NATIVE;
        *outNumModes = 1;
    } else {
        *outNumModes = 0;
    }
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_display_attribute(hwc2_device_t*, hwc2_display_t display,
                                  hwc2_config_t config, int32_t attribute,
                                  int32_t* outValue) {
    if (outValue == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    if (config != kActiveConfig) {
        return HWC2_ERROR_BAD_CONFIG;
    }

    switch (attribute) {
        case HWC2_ATTRIBUTE_WIDTH: *outValue = static_cast<int32_t>(xd::kDisplayWidth); break;
        case HWC2_ATTRIBUTE_HEIGHT: *outValue = static_cast<int32_t>(xd::kDisplayHeight); break;
        case HWC2_ATTRIBUTE_VSYNC_PERIOD: *outValue = static_cast<int32_t>(kVsyncPeriodNs); break;
        case HWC2_ATTRIBUTE_DPI_X:
        case HWC2_ATTRIBUTE_DPI_Y: *outValue = kDpi * 1000; break;
        case HWC2_ATTRIBUTE_CONFIG_GROUP: *outValue = 0; break;
        default: *outValue = -1; break;
    }
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_display_configs(hwc2_device_t*, hwc2_display_t display,
                                uint32_t* outNumConfigs, hwc2_config_t* outConfigs) {
    if (outNumConfigs == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    if (outConfigs == nullptr) {
        *outNumConfigs = 1;
        return HWC2_ERROR_NONE;
    }
    if (*outNumConfigs > 0) {
        outConfigs[0] = kActiveConfig;
        *outNumConfigs = 1;
    } else {
        *outNumConfigs = 0;
    }
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_display_name(hwc2_device_t*, hwc2_display_t display,
                             uint32_t* outSize, char* outName) {
    if (outSize == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    static constexpr char kName[] = "x11-droid";
    const uint32_t need = sizeof kName;
    if (outName == nullptr) {
        *outSize = need;
        return HWC2_ERROR_NONE;
    }
    const uint32_t n = need < *outSize ? need : *outSize;
    memcpy(outName, kName, n);
    *outSize = n;
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_display_requests(hwc2_device_t*, hwc2_display_t display,
                                 int32_t* outDisplayRequests,
                                 uint32_t* outNumElements, hwc2_layer_t*,
                                 int32_t*) {
    if (outDisplayRequests == nullptr || outNumElements == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    if (!g_display.validated) {
        return HWC2_ERROR_NOT_VALIDATED;
    }
    *outDisplayRequests = 0;
    *outNumElements = 0;
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_display_type(hwc2_device_t*, hwc2_display_t display,
                             int32_t* outType) {
    if (outType == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    *outType = HWC2_DISPLAY_TYPE_PHYSICAL;
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_doze_support(hwc2_device_t*, hwc2_display_t display,
                             int32_t* outSupport) {
    if (outSupport == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    *outSupport = 0;
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_hdr_capabilities(hwc2_device_t*, hwc2_display_t display,
                                 uint32_t* outNumTypes, int32_t*,
                                 float* outMaxLuminance,
                                 float* outMaxAverageLuminance,
                                 float* outMinLuminance) {
    if (outNumTypes == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    *outNumTypes = 0;
    if (outMaxLuminance != nullptr) {
        *outMaxLuminance = 0.0f;
    }
    if (outMaxAverageLuminance != nullptr) {
        *outMaxAverageLuminance = 0.0f;
    }
    if (outMinLuminance != nullptr) {
        *outMinLuminance = 0.0f;
    }
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_release_fences(hwc2_device_t*, hwc2_display_t display,
                               uint32_t* outNumElements, hwc2_layer_t*,
                               int32_t*) {
    if (outNumElements == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    *outNumElements = 0;
    return HWC2_ERROR_NONE;
}

int32_t hwc_get_display_vsync_period(hwc2_device_t*, hwc2_display_t display,
                                     hwc2_vsync_period_t* outVsyncPeriod) {
    if (outVsyncPeriod == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    *outVsyncPeriod = kVsyncPeriodNs;
    return HWC2_ERROR_NONE;
}

int32_t hwc_present_display(hwc2_device_t*, hwc2_display_t display,
                            int32_t* outPresentFence) {
    if (outPresentFence == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    *outPresentFence = -1;

    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    ++g_display.frames;

    if (g_display.power != HWC2_POWER_MODE_ON) {
        return HWC2_ERROR_NONE;
    }
    if (g_display.client_target == nullptr) {
        return HWC2_ERROR_NONE;
    }

    wait_fence(g_display.client_acquire_fence, &g_display.fence_timeouts);
    g_display.client_acquire_fence = -1;

    MappedBuffer fb;
    if (!g_gralloc.map(g_display.client_target, xd::kDisplayWidth,
                       xd::kDisplayHeight, &fb)) {
        ++g_display.map_failures;
        return HWC2_ERROR_NONE;
    }

    g_shm.present(fb.data, fb.stride_bytes, fb.format, fb.width, fb.height);
    GrallocMapper::unmap(&fb);

    /* The client target buffer is only valid until this present completes. */
    g_display.client_target = nullptr;
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_active_config(hwc2_device_t*, hwc2_display_t display,
                              hwc2_config_t config) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    if (config != kActiveConfig) {
        return HWC2_ERROR_BAD_CONFIG;
    }
    g_display.active_config = config;
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_client_target(hwc2_device_t*, hwc2_display_t display,
                              buffer_handle_t target, int32_t acquireFence,
                              int32_t, hwc_region_t) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        close_fence(acquireFence);
        return err;
    }
    close_fence(g_display.client_acquire_fence);
    g_display.client_target = target;
    g_display.client_acquire_fence = acquireFence;
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_color_mode(hwc2_device_t*, hwc2_display_t display, int32_t) {
    std::lock_guard<std::mutex> lk(g_lock);
    return display_error(display);
}

int32_t hwc_set_color_transform(hwc2_device_t*, hwc2_display_t display,
                                const float*, int32_t) {
    std::lock_guard<std::mutex> lk(g_lock);
    return display_error(display);
}

int32_t hwc_set_cursor_position(hwc2_device_t*, hwc2_display_t display,
                                hwc2_layer_t layer, int32_t, int32_t) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    return find_layer_locked(layer) != nullptr ? HWC2_ERROR_NONE
                                               : HWC2_ERROR_BAD_LAYER;
}

int32_t hwc_set_output_buffer(hwc2_device_t*, hwc2_display_t display,
                              buffer_handle_t, int32_t releaseFence) {
    close_fence(releaseFence);
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    return err == HWC2_ERROR_NONE ? HWC2_ERROR_UNSUPPORTED : err;
}

int32_t hwc_set_power_mode(hwc2_device_t*, hwc2_display_t display, int32_t mode) {
    if (mode < HWC2_POWER_MODE_OFF || mode > HWC2_POWER_MODE_DOZE_SUSPEND) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    g_display.power = static_cast<hwc2_power_mode_t>(mode);
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_vsync_enabled(hwc2_device_t*, hwc2_display_t display,
                              int32_t enabled) {
    if (enabled != HWC2_VSYNC_ENABLE && enabled != HWC2_VSYNC_DISABLE) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::thread* to_join = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        const int32_t err = display_error(display);
        if (err != HWC2_ERROR_NONE) {
            return err;
        }
        g_display.vsync_enabled = (enabled == HWC2_VSYNC_ENABLE);
        if (g_display.vsync_enabled && g_vsync_thread == nullptr) {
            g_vsync_stop = false;
            g_vsync_thread = new std::thread(&vsync_thread_main);
        } else if (!g_display.vsync_enabled && g_vsync_thread != nullptr) {
            g_vsync_stop = true;
            to_join = g_vsync_thread;
            g_vsync_thread = nullptr;
        }
        g_vsync_cv.notify_all();
    }
    if (to_join != nullptr) {
        to_join->join();
        delete to_join;
    }
    return HWC2_ERROR_NONE;
}

int32_t hwc_validate_display(hwc2_device_t*, hwc2_display_t display,
                             uint32_t* outNumTypes, uint32_t* outNumRequests) {
    if (outNumTypes == nullptr || outNumRequests == nullptr) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }

    g_display.changed_layers.clear();
    g_display.changed_types.clear();
    for (auto& layer : g_display.layers) {
        if (layer.composition != HWC2_COMPOSITION_CLIENT) {
            g_display.changed_layers.push_back(layer.id);
            g_display.changed_types.push_back(HWC2_COMPOSITION_CLIENT);
        }
    }
    g_display.validated = true;

    *outNumTypes = static_cast<uint32_t>(g_display.changed_layers.size());
    *outNumRequests = 0;
    return g_display.changed_layers.empty() ? HWC2_ERROR_NONE
                                            : HWC2_ERROR_HAS_CHANGES;
}

/* ------------------------------------------------------------------ layer */

int32_t hwc_set_layer_blend_mode(hwc2_device_t*, hwc2_display_t display,
                                 hwc2_layer_t layer, int32_t mode) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    LayerState* state = find_layer_locked(layer);
    if (state == nullptr) {
        return HWC2_ERROR_BAD_LAYER;
    }
    if (mode < HWC2_BLEND_MODE_NONE || mode > HWC2_BLEND_MODE_COVERAGE) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    state->blend_mode = static_cast<hwc2_blend_mode_t>(mode);
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_layer_buffer(hwc2_device_t*, hwc2_display_t display,
                             hwc2_layer_t layer, buffer_handle_t,
                             int32_t acquireFence) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        close_fence(acquireFence);
        return err;
    }
    if (find_layer_locked(layer) == nullptr) {
        close_fence(acquireFence);
        return HWC2_ERROR_BAD_LAYER;
    }
    /* All layers are client-composed: SurfaceFlinger owns the buffers. */
    close_fence(acquireFence);
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_layer_color(hwc2_device_t*, hwc2_display_t display,
                            hwc2_layer_t layer, hwc_color_t color) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    LayerState* state = find_layer_locked(layer);
    if (state == nullptr) {
        return HWC2_ERROR_BAD_LAYER;
    }
    state->color = color;
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_layer_composition_type(hwc2_device_t*, hwc2_display_t display,
                                       hwc2_layer_t layer, int32_t type) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    LayerState* state = find_layer_locked(layer);
    if (state == nullptr) {
        return HWC2_ERROR_BAD_LAYER;
    }
    if (type < HWC2_COMPOSITION_CLIENT || type > HWC2_COMPOSITION_SIDEBAND) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    state->composition = static_cast<hwc2_composition_t>(type);
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_layer_dataspace(hwc2_device_t*, hwc2_display_t display,
                                hwc2_layer_t layer, int32_t) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    return find_layer_locked(layer) != nullptr ? HWC2_ERROR_NONE
                                               : HWC2_ERROR_BAD_LAYER;
}

int32_t hwc_set_layer_display_frame(hwc2_device_t*, hwc2_display_t display,
                                    hwc2_layer_t layer, hwc_rect_t frame) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    LayerState* state = find_layer_locked(layer);
    if (state == nullptr) {
        return HWC2_ERROR_BAD_LAYER;
    }
    state->display_frame = frame;
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_layer_plane_alpha(hwc2_device_t*, hwc2_display_t display,
                                  hwc2_layer_t layer, float alpha) {
    if (!std::isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    LayerState* state = find_layer_locked(layer);
    if (state == nullptr) {
        return HWC2_ERROR_BAD_LAYER;
    }
    state->plane_alpha = alpha;
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_layer_sideband_stream(hwc2_device_t*, hwc2_display_t display,
                                      hwc2_layer_t layer,
                                      const native_handle_t*) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    return find_layer_locked(layer) != nullptr ? HWC2_ERROR_NONE
                                               : HWC2_ERROR_BAD_LAYER;
}

int32_t hwc_set_layer_source_crop(hwc2_device_t*, hwc2_display_t display,
                                  hwc2_layer_t layer, hwc_frect_t crop) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    LayerState* state = find_layer_locked(layer);
    if (state == nullptr) {
        return HWC2_ERROR_BAD_LAYER;
    }
    state->source_crop = crop;
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_layer_transform(hwc2_device_t*, hwc2_display_t display,
                                hwc2_layer_t layer, int32_t transform) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    LayerState* state = find_layer_locked(layer);
    if (state == nullptr) {
        return HWC2_ERROR_BAD_LAYER;
    }
    if (transform < 0 || transform > 7) {
        return HWC2_ERROR_BAD_PARAMETER;
    }
    state->transform = transform;
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_layer_visible_region(hwc2_device_t*, hwc2_display_t display,
                                     hwc2_layer_t layer, hwc_region_t) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    return find_layer_locked(layer) != nullptr ? HWC2_ERROR_NONE
                                               : HWC2_ERROR_BAD_LAYER;
}

int32_t hwc_set_layer_z_order(hwc2_device_t*, hwc2_display_t display,
                              hwc2_layer_t layer, uint32_t z) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    LayerState* state = find_layer_locked(layer);
    if (state == nullptr) {
        return HWC2_ERROR_BAD_LAYER;
    }
    state->z_order = z;
    return HWC2_ERROR_NONE;
}

int32_t hwc_set_layer_surface_damage(hwc2_device_t*, hwc2_display_t display,
                                     hwc2_layer_t layer, hwc_region_t) {
    std::lock_guard<std::mutex> lk(g_lock);
    const int32_t err = display_error(display);
    if (err != HWC2_ERROR_NONE) {
        return err;
    }
    return find_layer_locked(layer) != nullptr ? HWC2_ERROR_NONE
                                               : HWC2_ERROR_BAD_LAYER;
}

/* ------------------------------------------------------------- device ops */

#define XD_FN(descriptor, type_name, fn_name)                               \
    case descriptor: {                                                       \
        using Fn = HWC2_PFN_##type_name;                                     \
        Fn fn = hwc_##fn_name;                                               \
        return reinterpret_cast<hwc2_function_pointer_t>(fn);                \
    }

hwc2_function_pointer_t hwc_get_function(hwc2_device_t*, int32_t descriptor) {
    switch (descriptor) {
        XD_FN(HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES, ACCEPT_DISPLAY_CHANGES, accept_display_changes)
        XD_FN(HWC2_FUNCTION_CREATE_LAYER, CREATE_LAYER, create_layer)
        XD_FN(HWC2_FUNCTION_CREATE_VIRTUAL_DISPLAY, CREATE_VIRTUAL_DISPLAY, create_virtual_display)
        XD_FN(HWC2_FUNCTION_DESTROY_LAYER, DESTROY_LAYER, destroy_layer)
        XD_FN(HWC2_FUNCTION_DESTROY_VIRTUAL_DISPLAY, DESTROY_VIRTUAL_DISPLAY, destroy_virtual_display)
        XD_FN(HWC2_FUNCTION_DUMP, DUMP, dump)
        XD_FN(HWC2_FUNCTION_GET_ACTIVE_CONFIG, GET_ACTIVE_CONFIG, get_active_config)
        XD_FN(HWC2_FUNCTION_GET_CHANGED_COMPOSITION_TYPES, GET_CHANGED_COMPOSITION_TYPES, get_changed_composition_types)
        XD_FN(HWC2_FUNCTION_GET_CLIENT_TARGET_SUPPORT, GET_CLIENT_TARGET_SUPPORT, get_client_target_support)
        XD_FN(HWC2_FUNCTION_GET_COLOR_MODES, GET_COLOR_MODES, get_color_modes)
        XD_FN(HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE, GET_DISPLAY_ATTRIBUTE, get_display_attribute)
        XD_FN(HWC2_FUNCTION_GET_DISPLAY_CONFIGS, GET_DISPLAY_CONFIGS, get_display_configs)
        XD_FN(HWC2_FUNCTION_GET_DISPLAY_NAME, GET_DISPLAY_NAME, get_display_name)
        XD_FN(HWC2_FUNCTION_GET_DISPLAY_REQUESTS, GET_DISPLAY_REQUESTS, get_display_requests)
        XD_FN(HWC2_FUNCTION_GET_DISPLAY_TYPE, GET_DISPLAY_TYPE, get_display_type)
        XD_FN(HWC2_FUNCTION_GET_DOZE_SUPPORT, GET_DOZE_SUPPORT, get_doze_support)
        XD_FN(HWC2_FUNCTION_GET_HDR_CAPABILITIES, GET_HDR_CAPABILITIES, get_hdr_capabilities)
        XD_FN(HWC2_FUNCTION_GET_MAX_VIRTUAL_DISPLAY_COUNT, GET_MAX_VIRTUAL_DISPLAY_COUNT, get_max_virtual_display_count)
        XD_FN(HWC2_FUNCTION_GET_RELEASE_FENCES, GET_RELEASE_FENCES, get_release_fences)
        XD_FN(HWC2_FUNCTION_PRESENT_DISPLAY, PRESENT_DISPLAY, present_display)
        XD_FN(HWC2_FUNCTION_REGISTER_CALLBACK, REGISTER_CALLBACK, register_callback)
        XD_FN(HWC2_FUNCTION_SET_ACTIVE_CONFIG, SET_ACTIVE_CONFIG, set_active_config)
        XD_FN(HWC2_FUNCTION_SET_CLIENT_TARGET, SET_CLIENT_TARGET, set_client_target)
        XD_FN(HWC2_FUNCTION_SET_COLOR_MODE, SET_COLOR_MODE, set_color_mode)
        XD_FN(HWC2_FUNCTION_SET_COLOR_TRANSFORM, SET_COLOR_TRANSFORM, set_color_transform)
        XD_FN(HWC2_FUNCTION_SET_CURSOR_POSITION, SET_CURSOR_POSITION, set_cursor_position)
        XD_FN(HWC2_FUNCTION_SET_LAYER_BLEND_MODE, SET_LAYER_BLEND_MODE, set_layer_blend_mode)
        XD_FN(HWC2_FUNCTION_SET_LAYER_BUFFER, SET_LAYER_BUFFER, set_layer_buffer)
        XD_FN(HWC2_FUNCTION_SET_LAYER_COLOR, SET_LAYER_COLOR, set_layer_color)
        XD_FN(HWC2_FUNCTION_SET_LAYER_COMPOSITION_TYPE, SET_LAYER_COMPOSITION_TYPE, set_layer_composition_type)
        XD_FN(HWC2_FUNCTION_SET_LAYER_DATASPACE, SET_LAYER_DATASPACE, set_layer_dataspace)
        XD_FN(HWC2_FUNCTION_SET_LAYER_DISPLAY_FRAME, SET_LAYER_DISPLAY_FRAME, set_layer_display_frame)
        XD_FN(HWC2_FUNCTION_SET_LAYER_PLANE_ALPHA, SET_LAYER_PLANE_ALPHA, set_layer_plane_alpha)
        XD_FN(HWC2_FUNCTION_SET_LAYER_SIDEBAND_STREAM, SET_LAYER_SIDEBAND_STREAM, set_layer_sideband_stream)
        XD_FN(HWC2_FUNCTION_SET_LAYER_SOURCE_CROP, SET_LAYER_SOURCE_CROP, set_layer_source_crop)
        XD_FN(HWC2_FUNCTION_SET_LAYER_SURFACE_DAMAGE, SET_LAYER_SURFACE_DAMAGE, set_layer_surface_damage)
        XD_FN(HWC2_FUNCTION_SET_LAYER_TRANSFORM, SET_LAYER_TRANSFORM, set_layer_transform)
        XD_FN(HWC2_FUNCTION_SET_LAYER_VISIBLE_REGION, SET_LAYER_VISIBLE_REGION, set_layer_visible_region)
        XD_FN(HWC2_FUNCTION_SET_LAYER_Z_ORDER, SET_LAYER_Z_ORDER, set_layer_z_order)
        XD_FN(HWC2_FUNCTION_SET_OUTPUT_BUFFER, SET_OUTPUT_BUFFER, set_output_buffer)
        XD_FN(HWC2_FUNCTION_SET_POWER_MODE, SET_POWER_MODE, set_power_mode)
        XD_FN(HWC2_FUNCTION_SET_VSYNC_ENABLED, SET_VSYNC_ENABLED, set_vsync_enabled)
        XD_FN(HWC2_FUNCTION_VALIDATE_DISPLAY, VALIDATE_DISPLAY, validate_display)
        XD_FN(HWC2_FUNCTION_GET_DISPLAY_VSYNC_PERIOD, GET_DISPLAY_VSYNC_PERIOD, get_display_vsync_period)
        default:
            return nullptr;
    }
}

#undef XD_FN

void hwc_get_capabilities(hwc2_device_t*, uint32_t* outCount,
                          int32_t* outCapabilities) {
    if (outCount == nullptr) {
        return;
    }
    (void)outCapabilities;
    *outCount = 0;
}

int hwc_close(hw_device_t*) {
    std::thread* to_join = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_vsync_stop = true;
        g_display.vsync_enabled = false;
        to_join = g_vsync_thread;
        g_vsync_thread = nullptr;
    }
    g_vsync_cv.notify_all();
    if (to_join != nullptr) {
        to_join->join();
        delete to_join;
    }
    return 0;
}

int hwc_open(const hw_module_t* module, const char* id, hw_device_t** device) {
    if (module == nullptr || device == nullptr || id == nullptr ||
        strcmp(id, HWC_HARDWARE_COMPOSER) != 0) {
        return -EINVAL;
    }

    {
        std::lock_guard<std::mutex> lk(g_lock);
        reset_state_locked();
        g_vsync_stop = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_cb_lock);
        g_cb_hotplug = CallbackSlot {};
        g_cb_refresh = CallbackSlot {};
        g_cb_vsync = CallbackSlot {};
        g_cb_vsync24 = CallbackSlot {};
    }

    memset(&g_device, 0, sizeof g_device);
    g_device.common.tag = HARDWARE_DEVICE_TAG;
    g_device.common.version = HWC_DEVICE_API_VERSION_2_0;
    g_device.common.module = const_cast<hw_module_t*>(module);
    g_device.common.close = &hwc_close;
    g_device.getCapabilities = &hwc_get_capabilities;
    g_device.getFunction = &hwc_get_function;

    *device = &g_device.common;
    XD_LOGI("hwcomposer.x11 ready (720x1280, all layers client-composed)");
    return 0;
}

hw_module_methods_t g_module_methods = {
    &hwc_open,
};

} // namespace

extern "C" {
__attribute__((visibility("default")))
hwc_module_t HAL_MODULE_INFO_SYM = {
    {
        HARDWARE_MODULE_TAG,
        HWC_MODULE_API_VERSION_0_1,
        HARDWARE_HAL_API_VERSION,
        HWC_HARDWARE_MODULE_ID,
        "x11droid hwcomposer",
        "x11-droid",
        &g_module_methods,
        nullptr,
        {},
    },
};
} // extern "C"
