/* language: C11, file: host-daemon/fb_proto.h, target: Linux x86_64
 * Shared contract between the X11 daemon and test_producer:
 *   - framebuffer POSIX shm /dev/shm/x11droid_fb
 *   - refresh trigger messages over a Unix domain socket
 */
#ifndef XD_FB_PROTO_H
#define XD_FB_PROTO_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define XD_FB_NAME          "/x11droid_fb"          /* shm_open name */
#define XD_FB_PATH          "/dev/shm/x11droid_fb"  /* for messages/logs */
#define XD_SOCK_PATH        "/tmp/x11droid.sock"

#define XD_FB_MAGIC         0x31424458u  /* "XDB1" little-endian */
#define XD_MSG_MAGIC        0x31474D58u  /* "XMG1" little-endian */
#define XD_FB_VERSION       1u
#define XD_PIXEL_BGRA8888   0u
#define XD_HEADER_BYTES     64u

typedef struct {
    uint32_t        magic;        /* written last during initialization */
    uint32_t        version;
    uint32_t        width;
    uint32_t        height;
    uint32_t        stride;       /* bytes per row, = width * 4 */
    uint32_t        bpp;          /* 4 */
    uint32_t        pixel_format; /* XD_PIXEL_BGRA8888 */
    _Atomic uint32_t seq;         /* incremented after a frame is fully written */
    uint32_t        reserved[8];
} xd_fb_header;

_Static_assert(sizeof(xd_fb_header) == XD_HEADER_BYTES, "xd_fb_header must be 64 bytes");

/* Sent by the producer every frame; the daemon renders when seq changes. */
typedef struct {
    uint32_t magic;
    uint32_t seq;
} xd_msg;

_Static_assert(sizeof(xd_msg) == 8, "xd_msg must be 8 bytes");

#define XD_FB_SIZE(w, h)  (XD_HEADER_BYTES + (size_t)(w) * (size_t)(h) * 4u)
#define XD_FB_PIXELS(hdr) ((uint8_t *)(hdr) + XD_HEADER_BYTES)

#endif /* XD_FB_PROTO_H */
