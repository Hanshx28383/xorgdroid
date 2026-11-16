/* language: C++17, file: android/hwcomposer/xd_gralloc.cpp
 * Map client target buffers: gralloc0 lock path, dma-buf mmap fallback
 * for minigbm/cros_gralloc handles (magic 0xABCDDCBA).
 */
#include "xd_gralloc.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cutils/native_handle.h>
#include <system/graphics.h>

#include "xd_log.h"

namespace xd {
namespace {

constexpr uint32_t kCrosGrallocMagic = 0xABCDDCBAu;

/* cros_gralloc_handle layout (minigbm, Android 10-13). Packed; do not change. */
struct CrosGrallocHandle {
    native_handle_t base;
    int32_t fds[5];
    uint32_t strides[4];
    uint32_t offsets[4];
    uint32_t sizes[4];
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t format_modifier;
    uint64_t use_flags;
    uint32_t magic;
    uint32_t pixel_stride;
    int32_t droid_format;
    int32_t usage;
    uint32_t num_planes;
    uint64_t reserved_region_size;
    uint64_t total_size;
    uint32_t name_offset;
} __attribute__((packed));

struct DmaBufSync {
    uint64_t flags;
};

constexpr uint64_t kDmaBufSyncRead = 1ull << 0;
constexpr uint64_t kDmaBufSyncStart = 0ull << 2;
constexpr uint64_t kDmaBufSyncEnd = 1ull << 2;
constexpr unsigned long kDmaBufSyncIoctl =
    static_cast<unsigned long>(_IOW('b', 0, struct DmaBufSync));

struct LockCookie {
    const gralloc_module_t* module;
    buffer_handle_t handle;
    bool registered;
};

struct MmapCookie {
    void* base;
    size_t size;
    int fd;
};

void release_lock(void* p) {
    auto* c = static_cast<LockCookie*>(p);
    if (c->module != nullptr && c->module->unlock != nullptr) {
        c->module->unlock(c->module, c->handle);
    }
    if (c->registered && c->module != nullptr &&
        c->module->unregisterBuffer != nullptr) {
        c->module->unregisterBuffer(c->module, c->handle);
    }
    delete c;
}

void release_mmap(void* p) {
    auto* c = static_cast<MmapCookie*>(p);
    if (c->fd >= 0) {
        DmaBufSync sync {kDmaBufSyncRead | kDmaBufSyncEnd};
        ioctl(c->fd, kDmaBufSyncIoctl, &sync);
    }
    munmap(c->base, c->size);
    delete c;
}

const CrosGrallocHandle* as_cros(buffer_handle_t handle) {
    const auto* h = reinterpret_cast<const CrosGrallocHandle*>(handle);
    if (h == nullptr || h->magic != kCrosGrallocMagic) {
        return nullptr;
    }
    return h;
}

uint32_t env_stride_pixels() {
    const char* v = getenv("X11DROID_HWC_STRIDE");
    if (v == nullptr || *v == '\0') {
        return 0;
    }
    char* end = nullptr;
    const unsigned long n = strtoul(v, &end, 0);
    if (end == v || n > 32768ul) {
        return 0;
    }
    return static_cast<uint32_t>(n);
}

int32_t sane_format(int32_t android_format) {
    switch (android_format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_RGBX_8888:
            return android_format;
        default:
            /* HWC2 contract: the client target must be RGBA_8888. */
            return HAL_PIXEL_FORMAT_RGBA_8888;
    }
}

} // namespace

struct GrallocMapper::Impl {
    void* lib = nullptr;
    const gralloc_module_t* module = nullptr;
    bool loaded = false;
    bool load_logged = false;

    void load() {
        if (loaded) {
            return;
        }
        loaded = true;

        lib = dlopen("libhardware.so", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) {
            return;
        }
        using HwGetModuleFn = int (*)(const char*, const struct hw_module_t**);
        auto* fn = reinterpret_cast<HwGetModuleFn>(dlsym(lib, "hw_get_module"));
        if (fn == nullptr) {
            return;
        }
        const struct hw_module_t* mod = nullptr;
        if (fn(GRALLOC_HARDWARE_MODULE_ID, &mod) == 0 && mod != nullptr &&
            mod->tag == HARDWARE_MODULE_TAG) {
            module = reinterpret_cast<const gralloc_module_t*>(mod);
        }
    }
};

GrallocMapper::GrallocMapper() : impl_(new Impl()) {}

GrallocMapper::~GrallocMapper() {
    if (impl_->lib != nullptr) {
        dlclose(impl_->lib);
    }
}

bool GrallocMapper::map(buffer_handle_t handle, uint32_t width, uint32_t height,
                        MappedBuffer* out) {
    if (out == nullptr || handle == nullptr || width == 0 || height == 0) {
        return false;
    }
    *out = MappedBuffer {};

    impl_->load();
    const CrosGrallocHandle* cros = as_cros(handle);

    /* Primary path: gralloc0 lock, used by all legacy implementations. */
    if (impl_->module != nullptr && impl_->module->lock != nullptr) {
        bool registered = false;
        if (impl_->module->registerBuffer != nullptr) {
            registered = impl_->module->registerBuffer(impl_->module, handle) == 0;
        }

        void* vaddr = nullptr;
        const int rc = impl_->module->lock(
            impl_->module, handle, GRALLOC_USAGE_SW_READ_OFTEN, 0, 0,
            static_cast<int>(width), static_cast<int>(height), &vaddr);
        if (rc == 0 && vaddr != nullptr) {
            uint32_t stride_px = 0;
            int32_t format = HAL_PIXEL_FORMAT_RGBA_8888;
            if (cros != nullptr) {
                stride_px = cros->pixel_stride;
                format = sane_format(cros->droid_format);
            }
            if (stride_px == 0) {
                stride_px = env_stride_pixels();
            }
            if (stride_px == 0) {
                stride_px = width;
            }
            if (stride_px >= width) {
                auto* cookie = new LockCookie {impl_->module, handle, registered};
                out->data = static_cast<const uint8_t*>(vaddr);
                out->stride_bytes = stride_px * 4u;
                out->width = width;
                out->height = height;
                out->format = format;
                out->cookie = cookie;
                out->release = &release_lock;
                impl_->load_logged = false;
                return true;
            }
            impl_->module->unlock(impl_->module, handle);
        }

        if (registered && impl_->module->unregisterBuffer != nullptr) {
            impl_->module->unregisterBuffer(impl_->module, handle);
        }
    }

    /* Fallback: minigbm dma-buf can be mapped directly without the gralloc lock. */
    if (cros != nullptr && cros->num_planes >= 1 && cros->fds[0] >= 0 &&
        cros->sizes[0] > 0 && cros->offsets[0] < cros->sizes[0]) {
        uint32_t stride_px = cros->pixel_stride;
        if (stride_px == 0 && cros->strides[0] >= width * 4u) {
            stride_px = cros->strides[0] / 4u;
        }
        if (stride_px == 0) {
            stride_px = env_stride_pixels();
        }
        if (stride_px >= width) {
            const size_t offset = cros->offsets[0];
            const size_t plane_end = offset + cros->sizes[0];
            const size_t size = cros->total_size >= plane_end ? cros->total_size
                                                             : plane_end;
            const size_t need = offset + static_cast<size_t>(stride_px) * 4u * height;
            if (need <= size) {
                void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED,
                                  cros->fds[0], 0);
                if (base != MAP_FAILED) {
                    DmaBufSync sync {kDmaBufSyncRead | kDmaBufSyncStart};
                    ioctl(cros->fds[0], kDmaBufSyncIoctl, &sync);

                    auto* cookie =
                        new MmapCookie {base, size, cros->fds[0]};
                    out->data = static_cast<const uint8_t*>(base) + offset;
                    out->stride_bytes = stride_px * 4u;
                    out->width = width;
                    out->height = height;
                    out->format = sane_format(cros->droid_format);
                    out->cookie = cookie;
                    out->release = &release_mmap;
                    impl_->load_logged = false;
                    return true;
                }
            }
        }
    }

    if (!impl_->load_logged) {
        XD_LOGE("cannot map client target (gralloc module %s, handle %s)",
                impl_->module != nullptr ? "present" : "absent",
                cros != nullptr ? "minigbm" : "non-minigbm");
        impl_->load_logged = true;
    }
    return false;
}

void GrallocMapper::unmap(MappedBuffer* buffer) {
    if (buffer == nullptr) {
        return;
    }
    if (buffer->cookie != nullptr && buffer->release != nullptr) {
        buffer->release(buffer->cookie);
    }
    *buffer = MappedBuffer {};
}

} // namespace xd
