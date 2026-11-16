/* language: C++17, file: android/hwcomposer/xd_fb_layout.h
 * C++ (NDK) mirror of the host-daemon fb_proto.h layout.
 * Do not reorder fields: the header is serialized across processes.
 */
#ifndef XD_FB_LAYOUT_H
#define XD_FB_LAYOUT_H

#include <cstddef>
#include <cstdint>

namespace xd {

constexpr uint32_t kFbMagic = 0x31424458u;      /* "XDB1" */
constexpr uint32_t kMsgMagic = 0x31474D58u;     /* "XMG1" */
constexpr uint32_t kFbVersion = 1u;
constexpr uint32_t kPixelBgra8888 = 0u;
constexpr uint32_t kHeaderBytes = 64u;
constexpr uint32_t kDisplayWidth = 720u;
constexpr uint32_t kDisplayHeight = 1280u;

struct FbHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;        /* bytes per row */
    uint32_t bpp;
    uint32_t pixel_format;
    uint32_t seq;           /* accessed with __atomic_* */
    uint32_t reserved[8];
};

struct Msg {
    uint32_t magic;
    uint32_t seq;
};

constexpr size_t kFbBytes = kHeaderBytes +
                            static_cast<size_t>(kDisplayWidth) * kDisplayHeight * 4u;

static_assert(sizeof(FbHeader) == kHeaderBytes, "FbHeader must be 64 bytes");
static_assert(sizeof(Msg) == 8, "Msg must be 8 bytes");
static_assert(offsetof(FbHeader, seq) == 28, "seq offset changed");

} // namespace xd

#endif /* XD_FB_LAYOUT_H */
