/* language: C++17, file: android/hwcomposer/xd_shm.h
 * Shared framebuffer writer + refresh trigger for the host daemon.
 */
#ifndef XD_SHM_H
#define XD_SHM_H

#include <cstddef>
#include <cstdint>

#include "xd_fb_layout.h"

namespace xd {

class ShmSink {
  public:
    ShmSink();
    ~ShmSink();

    ShmSink(const ShmSink&) = delete;
    ShmSink& operator=(const ShmSink&) = delete;

    /* Copy one frame to /dev/shm/x11droid_fb, then send xd_msg.
     * src_format uses HAL_PIXEL_FORMAT_* constants.
     * false = frame dropped (daemon not ready / bad geometry / map failed).
     */
    bool present(const void* src, uint32_t src_stride_bytes, int32_t src_format,
                 uint32_t width, uint32_t height);

    uint64_t frames_presented() const { return frames_presented_; }
    uint64_t frames_dropped() const { return frames_dropped_; }

  private:
    bool ensure_mapping();
    void close_mapping();
    bool ensure_socket();
    void close_socket();
    void send_refresh(uint32_t seq);
    bool copy_frame(const void* src, uint32_t src_stride_bytes,
                    int32_t src_format, uint32_t width, uint32_t height);

    int fd_ = -1;
    void* map_ = nullptr;
    size_t map_size_ = 0;
    FbHeader* hdr_ = nullptr;
    int sock_ = -1;

    bool not_ready_logged_ = false;
    bool geometry_logged_ = false;
    uint64_t frames_presented_ = 0;
    uint64_t frames_dropped_ = 0;
};

} // namespace xd

#endif /* XD_SHM_H */
