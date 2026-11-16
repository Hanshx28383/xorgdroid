/* language: C++17, file: android/hwcomposer/xd_shm.cpp
 * HWC frame sink -> /dev/shm/x11droid_fb + /tmp/x11droid.sock signal.
 */
#include "xd_shm.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <system/graphics.h>

#include "xd_log.h"

namespace xd {
namespace {

constexpr const char* kFbPath = "/dev/shm/x11droid_fb";
constexpr const char* kSockPath = "/tmp/x11droid.sock";

bool same_file(int fd, const char* path) {
    struct stat path_st {};
    struct stat fd_st {};
    if (stat(path, &path_st) != 0 || fstat(fd, &fd_st) != 0) {
        return false;
    }
    return path_st.st_dev == fd_st.st_dev && path_st.st_ino == fd_st.st_ino;
}

} // namespace

ShmSink::ShmSink() = default;

ShmSink::~ShmSink() {
    close_socket();
    close_mapping();
}

void ShmSink::close_socket() {
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}

void ShmSink::close_mapping() {
    if (map_ != nullptr) {
        munmap(map_, map_size_);
        map_ = nullptr;
    }
    map_size_ = 0;
    hdr_ = nullptr;
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

bool ShmSink::ensure_mapping() {
    if (fd_ >= 0 && !same_file(fd_, kFbPath)) {
        /* Daemon restart: shm_unlink + new segment. */
        close_mapping();
        not_ready_logged_ = false;
    }

    if (fd_ < 0) {
        fd_ = open(kFbPath, O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            if (!not_ready_logged_) {
                XD_LOGW("waiting for host-daemon: %s missing (%s)", kFbPath,
                        strerror(errno));
                not_ready_logged_ = true;
            }
            return false;
        }

        struct stat st {};
        if (fstat(fd_, &st) != 0 || st.st_size < static_cast<off_t>(kHeaderBytes)) {
            close_mapping();
            return false;
        }
        map_size_ = static_cast<size_t>(st.st_size);
        map_ = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (map_ == MAP_FAILED) {
            map_ = nullptr;
            XD_LOGE("mmap %s failed: %s", kFbPath, strerror(errno));
            close_mapping();
            return false;
        }
        hdr_ = static_cast<FbHeader*>(map_);
    }

    if (__atomic_load_n(&hdr_->magic, __ATOMIC_ACQUIRE) != kFbMagic) {
        if (!not_ready_logged_) {
            XD_LOGW("waiting for framebuffer header from host-daemon");
            not_ready_logged_ = true;
        }
        return false;
    }
    return true;
}

bool ShmSink::ensure_socket() {
    if (sock_ >= 0) {
        return true;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    const int n = snprintf(addr.sun_path, sizeof addr.sun_path, "%s", kSockPath);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof addr.sun_path) {
        close(fd);
        return false;
    }
    const socklen_t len = static_cast<socklen_t>(
        offsetof(struct sockaddr_un, sun_path) + static_cast<size_t>(n) + 1);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), len) != 0) {
        close(fd);
        return false;
    }

    struct timeval tv {};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sock_ = fd;
    return true;
}

void ShmSink::send_refresh(uint32_t seq) {
    if (!ensure_socket()) {
        return;
    }

    const Msg msg {kMsgMagic, seq};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&msg);
    size_t left = sizeof msg;
    while (left > 0) {
        const ssize_t n = send(sock_, p, left, MSG_NOSIGNAL);
        if (n > 0) {
            p += n;
            left -= static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        /* Daemon died or stalled: disconnect; the next frame reconnects. */
        close_socket();
        return;
    }
}

bool ShmSink::copy_frame(const void* src, uint32_t src_stride_bytes,
                         int32_t src_format, uint32_t width, uint32_t height) {
    const uint32_t dst_stride = hdr_->stride;
    uint8_t* dst = static_cast<uint8_t*>(map_) + kHeaderBytes;
    const uint8_t* s = static_cast<const uint8_t*>(src);

    if (src_format == HAL_PIXEL_FORMAT_BGRA_8888) {
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(dst + static_cast<size_t>(y) * dst_stride,
                   s + static_cast<size_t>(y) * src_stride_bytes, width * 4u);
        }
        return true;
    }

    if (src_format != HAL_PIXEL_FORMAT_RGBA_8888 &&
        src_format != HAL_PIXEL_FORMAT_RGBX_8888) {
        if (!geometry_logged_) {
            XD_LOGE("client target format 0x%x unsupported (need RGBA/BGRA 8888)",
                    src_format);
            geometry_logged_ = true;
        }
        return false;
    }

    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t* sp = reinterpret_cast<const uint32_t*>(
            s + static_cast<size_t>(y) * src_stride_bytes);
        uint32_t* dp = reinterpret_cast<uint32_t*>(
            dst + static_cast<size_t>(y) * dst_stride);
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t v = sp[x];
            uint32_t out = (v & 0xFF00FF00u) |
                           ((v & 0x000000FFu) << 16) |
                           ((v & 0x00FF0000u) >> 16);
            if (src_format == HAL_PIXEL_FORMAT_RGBX_8888) {
                out |= 0xFF000000u;
            }
            dp[x] = out;
        }
    }
    return true;
}

bool ShmSink::present(const void* src, uint32_t src_stride_bytes, int32_t src_format,
                      uint32_t width, uint32_t height) {
    if (src == nullptr || width == 0 || height == 0 ||
        src_stride_bytes < width * 4u) {
        ++frames_dropped_;
        return false;
    }

    if (!ensure_mapping()) {
        ++frames_dropped_;
        return false;
    }

    if (hdr_->version != kFbVersion || hdr_->bpp != 4u ||
        hdr_->width != width || hdr_->height != height ||
        hdr_->stride < width * 4u ||
        map_size_ < kHeaderBytes + static_cast<size_t>(hdr_->stride) * height) {
        if (!geometry_logged_) {
            XD_LOGE("shm geometry mismatch: hdr %ux%u stride %u vs frame %ux%u",
                    hdr_->width, hdr_->height, hdr_->stride, width, height);
            geometry_logged_ = true;
        }
        ++frames_dropped_;
        return false;
    }

    if (!copy_frame(src, src_stride_bytes, src_format, width, height)) {
        ++frames_dropped_;
        return false;
    }

    /* Pixels are fully written before seq increments (release). */
    const uint32_t seq = __atomic_add_fetch(&hdr_->seq, 1u, __ATOMIC_RELEASE);
    send_refresh(seq);

    geometry_logged_ = false;
    not_ready_logged_ = false;
    ++frames_presented_;
    return true;
}

} // namespace xd
