/* language: C++17, file: android/hwcomposer/xd_gralloc.h
 * CPU access to SurfaceFlinger client target buffers.
 */
#ifndef XD_GRALLOC_H
#define XD_GRALLOC_H

#include <cstdint>
#include <memory>

#include <hardware/gralloc.h>

namespace xd {

struct MappedBuffer {
    const uint8_t* data = nullptr;
    uint32_t stride_bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t format = 0;             /* HAL_PIXEL_FORMAT_* */
    void* cookie = nullptr;
    void (*release)(void* cookie) = nullptr;
};

class GrallocMapper {
  public:
    GrallocMapper();
    ~GrallocMapper();

    GrallocMapper(const GrallocMapper&) = delete;
    GrallocMapper& operator=(const GrallocMapper&) = delete;

    bool map(buffer_handle_t handle, uint32_t width, uint32_t height,
             MappedBuffer* out);
    static void unmap(MappedBuffer* buffer);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xd

#endif /* XD_GRALLOC_H */
