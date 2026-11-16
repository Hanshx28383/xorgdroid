# Architecture & Data Flows

## Overview

```
Android app / SurfaceFlinger
        |
        |  HWC2                         GLES 2.0
        v                                 v
hwcomposer.x11.so                 libGLES_x11droid.so
        |                                 |
        | /dev/shm/x11droid_fb            | /tmp/x11droid_gl.sock
        | /tmp/x11droid.sock              |
        v                                 v
                 x11droid-daemon
           X11 window + GLX context
                  |
                  v
             Xorg 720x1280 window
```

## Framebuffer Path (HWC2)

1. SurfaceFlinger composes all layers into client target (forced `HWC2_COMPOSITION_CLIENT`)
2. `hwcomposer.x11.so` maps client target buffer via gralloc/minigbm
3. Converts RGBA/BGRA → BGRA8888 in shared memory `/dev/shm/x11droid_fb`
4. Sends `xd_msg` (magic `XMG1`, seq) to `/tmp/x11droid.sock`
5. Host daemon presents via:
   - **MIT-SHM** (XShmPutImage) — preferred, zero-copy
   - **XPutImage** — CPU fallback
   - **GLX** — uploads to texture, draws with GPU (requires NVIDIA 470 + GLX)

## GLES Streaming Path

1. Android app calls GLES 2.0 via `libGLES_x11droid.so` (LD_PRELOAD)
2. Client serializes commands + variable payloads → `/tmp/x11droid_gl.sock`
3. `gles_server` thread in daemon receives, executes in same GLX context
4. Mutex `glx_lock()/glx_unlock()` serializes with window composition
5. GLSL ES 1.00 shaders translated to desktop GLSL 1.20 on the fly

## Key Design Points

- No Wayland/weston/wlroots/waydroid
- No GBM/DMA-BUF on host — pixels via shared memory
- HWC2 forces all layers `CLIENT`; HAL only copies final composition
- GLES server uses same GLX context as window composition, serialized by mutex + make-current per command
- Shader translation: strip `#version`/`#extension`/precision qualifiers; desktop GLSL 1.20 accepts `attribute`/`varying`/`texture2D`/`gl_FragColor`

## Shared Memory Layout

Header (64 bytes, `XDB1` magic):
```
magic(4) version(4) width(4) height(4) stride(4) bpp(4) pixel_format(4) seq(4) reserved[8](32)
```
Pixels follow at `header_size + ncolors*12` (XWD) or directly after header (internal).
Size: 64 + 720×1280×4 = 3,686,464 bytes.