#ifndef GLX_RENDER_H
#define GLX_RENDER_H

#include <stdbool.h>

#include <X11/Xlib.h>

/*
 * Composites the Android framebuffer with desktop OpenGL (GLX) in the host X11 window.
 * The NVIDIA 470 driver provides GLX, so the BGRA8888 buffer in shared memory is
 * uploaded to an OpenGL texture and drawn full-screen by the GPU — no CPU copy
 * to the screen, no GBM/DMA-BUF, no Wayland.
 *
 * All GL/GLX functions are resolved at runtime from libGL.so.1, so this file does
 * not require OpenGL development headers (GL/gl.h, GL/glx.h) on the build host.
 */

bool glx_create(Display *dpy, Window win);
void glx_draw(const void *pixels, int width, int height, int stride);
void glx_destroy(void);
bool glx_ready(void);

/* Serializes GLX context use across threads (main draw + GLES server).
 * Must be called in pairs; lock makes the context current on the calling thread. */
void glx_lock(void);
void glx_unlock(void);

#endif /* GLX_RENDER_H */
