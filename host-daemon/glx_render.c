/* language: C11, file: host-daemon/glx_render.c, target: Linux Xorg, gcc
 * Minimal GLX/OpenGL binding via dlopen("libGL.so.1") — no GL/gl.h or
 * GL/glx.h headers are required on the build host. Used to composite the
 * Android framebuffer (BGRA8888) through the NVIDIA 470 GPU in the X11 window.
 */
#include "glx_render.h"
#include <pthread.h>
#include "gles_stream/gles_server.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xutil.h>

/* ---------------- OpenGL / GLX constants (no GL headers) ---------------- */

#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_NONE                           0
#define GL_COLOR_BUFFER_BIT               0x00004000u
#define GL_TEXTURE_2D                     0x0DE1
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_LINEAR                         0x2601
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_BGRA_EXT                       0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV       0x8365
#define GL_RGBA                           0x1907
#define GL_PROJECTION                     0x1701
#define GL_MODELVIEW                      0x1700
#define GL_QUADS                          0x0007

#define GLX_RGBA                          1
#define GLX_DOUBLEBUFFER                  5
#define GLX_RED_SIZE                      8
#define GLX_GREEN_SIZE                    9
#define GLX_BLUE_SIZE                     10
#define GLX_DEPTH_SIZE                    11
#define GLX_USE_GL                        1

typedef void             GLvoid;
typedef unsigned char    GLboolean;
typedef unsigned int     GLenum;
typedef unsigned int     GLbitfield;
typedef int              GLint;
typedef int              GLsizei;
typedef unsigned int     GLuint;
typedef float            GLfloat;
typedef double           GLdouble;
typedef unsigned char    GLubyte;
typedef long             GLintptr;
typedef long             GLsizeiptr;

typedef unsigned long    GLXDrawable;
struct __GLXContextRec;
typedef struct __GLXContextRec *GLXContext;

typedef void (*GLVPPROC)(void);

/* ---------------- GLX function pointers ---------------- */

typedef XVisualInfo *(*PFN_glXChooseVisual)(Display *, int, int *);
typedef GLXContext    (*PFN_glXCreateContext)(Display *, XVisualInfo *, GLXContext, Bool);
typedef void          (*PFN_glXDestroyContext)(Display *, GLXContext);
typedef Bool          (*PFN_glXMakeCurrent)(Display *, GLXDrawable, GLXContext);
typedef void          (*PFN_glXSwapBuffers)(Display *, GLXDrawable);
typedef Bool          (*PFN_glXQueryExtension)(Display *, int *, int *);
typedef GLVPPROC      (*PFN_glXGetProcAddressARB)(const GLubyte *);

/* ---------------- OpenGL function pointers (resolved at runtime) ---------------- */

typedef void (*PFN_glClearColor)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
typedef void (*PFN_glClear)(GLbitfield mask);
typedef void (*PFN_glViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void (*PFN_glMatrixMode)(GLenum mode);
typedef void (*PFN_glLoadIdentity)(void);
typedef void (*PFN_glOrtho)(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
typedef void (*PFN_glEnable)(GLenum cap);
typedef void (*PFN_glDisable)(GLenum cap);
typedef void (*PFN_glBindTexture)(GLenum target, GLuint texture);
typedef void (*PFN_glTexParameteri)(GLenum target, GLenum pname, GLint param);
typedef void (*PFN_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels);
typedef void (*PFN_glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
typedef void (*PFN_glGenTextures)(GLsizei n, GLuint *textures);
typedef void (*PFN_glDeleteTextures)(GLsizei n, const GLuint *textures);
typedef void (*PFN_glBegin)(GLenum mode);
typedef void (*PFN_glEnd)(void);
typedef void (*PFN_glTexCoord2f)(GLfloat s, GLfloat t);
typedef void (*PFN_glVertex2f)(GLfloat x, GLfloat y);
typedef void (*PFN_glFinish)(void);
typedef GLenum (*PFN_glGetError)(void);

/* ---------------- internal state ---------------- */

static void           *g_gl_handle = NULL;
static PFN_glXChooseVisual    p_glXChooseVisual;
static PFN_glXCreateContext   p_glXCreateContext;
static PFN_glXDestroyContext  p_glXDestroyContext;
static PFN_glXMakeCurrent     p_glXMakeCurrent;
static PFN_glXSwapBuffers     p_glXSwapBuffers;
static PFN_glXQueryExtension  p_glXQueryExtension;
static PFN_glXGetProcAddressARB p_glXGetProcAddressARB;

static PFN_glClearColor     p_glClearColor;
static PFN_glClear          p_glClear;
static PFN_glViewport       p_glViewport;
static PFN_glMatrixMode     p_glMatrixMode;
static PFN_glLoadIdentity   p_glLoadIdentity;
static PFN_glOrtho          p_glOrtho;
static PFN_glEnable         p_glEnable;
static PFN_glDisable        p_glDisable;
static PFN_glBindTexture    p_glBindTexture;
static PFN_glTexParameteri  p_glTexParameteri;
static PFN_glTexImage2D     p_glTexImage2D;
static PFN_glTexSubImage2D  p_glTexSubImage2D;
static PFN_glGenTextures    p_glGenTextures;
static PFN_glDeleteTextures p_glDeleteTextures;
static PFN_glBegin          p_glBegin;
static PFN_glEnd            p_glEnd;
static PFN_glTexCoord2f     p_glTexCoord2f;
static PFN_glVertex2f       p_glVertex2f;
static PFN_glFinish         p_glFinish;
static PFN_glGetError       p_glGetError;

static Display    *g_dpy = NULL;
static GLXDrawable g_win = 0;
static GLXContext  g_ctx = NULL;
static GLuint      g_tex = 0;
static int         g_tex_w = 0;
static int         g_tex_h = 0;
static bool        g_ready = false;
static pthread_mutex_t g_glx_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *glsym(const char *name)
{
    void *p = dlsym(g_gl_handle, name);
    if (!p && p_glXGetProcAddressARB)
        p = p_glXGetProcAddressARB((const GLubyte *)name);
    return p;
}

static bool resolve_glx(void)
{
    p_glXChooseVisual    = (PFN_glXChooseVisual)    dlsym(g_gl_handle, "glXChooseVisual");
    p_glXCreateContext   = (PFN_glXCreateContext)   dlsym(g_gl_handle, "glXCreateContext");
    p_glXDestroyContext  = (PFN_glXDestroyContext)  dlsym(g_gl_handle, "glXDestroyContext");
    p_glXMakeCurrent     = (PFN_glXMakeCurrent)     dlsym(g_gl_handle, "glXMakeCurrent");
    p_glXSwapBuffers     = (PFN_glXSwapBuffers)     dlsym(g_gl_handle, "glXSwapBuffers");
    p_glXQueryExtension  = (PFN_glXQueryExtension)  dlsym(g_gl_handle, "glXQueryExtension");
    p_glXGetProcAddressARB = (PFN_glXGetProcAddressARB)dlsym(g_gl_handle, "glXGetProcAddressARB");
    return p_glXChooseVisual && p_glXCreateContext && p_glXDestroyContext &&
           p_glXMakeCurrent && p_glXSwapBuffers && p_glXQueryExtension &&
           p_glXGetProcAddressARB;
}

static bool resolve_gl(void)
{
    p_glClearColor     = (PFN_glClearColor)     glsym("glClearColor");
    p_glClear          = (PFN_glClear)          glsym("glClear");
    p_glViewport       = (PFN_glViewport)       glsym("glViewport");
    p_glMatrixMode     = (PFN_glMatrixMode)     glsym("glMatrixMode");
    p_glLoadIdentity   = (PFN_glLoadIdentity)   glsym("glLoadIdentity");
    p_glOrtho          = (PFN_glOrtho)          glsym("glOrtho");
    p_glEnable         = (PFN_glEnable)         glsym("glEnable");
    p_glDisable        = (PFN_glDisable)        glsym("glDisable");
    p_glBindTexture    = (PFN_glBindTexture)    glsym("glBindTexture");
    p_glTexParameteri  = (PFN_glTexParameteri)  glsym("glTexParameteri");
    p_glTexImage2D     = (PFN_glTexImage2D)     glsym("glTexImage2D");
    p_glTexSubImage2D  = (PFN_glTexSubImage2D)  glsym("glTexSubImage2D");
    p_glGenTextures    = (PFN_glGenTextures)    glsym("glGenTextures");
    p_glDeleteTextures = (PFN_glDeleteTextures) glsym("glDeleteTextures");
    p_glBegin          = (PFN_glBegin)          glsym("glBegin");
    p_glEnd            = (PFN_glEnd)            glsym("glEnd");
    p_glTexCoord2f     = (PFN_glTexCoord2f)     glsym("glTexCoord2f");
    p_glVertex2f       = (PFN_glVertex2f)       glsym("glVertex2f");
    p_glFinish         = (PFN_glFinish)         glsym("glFinish");
    p_glGetError       = (PFN_glGetError)       glsym("glGetError");
    return p_glClearColor && p_glClear && p_glViewport && p_glMatrixMode &&
           p_glLoadIdentity && p_glOrtho && p_glEnable && p_glDisable &&
           p_glBindTexture && p_glTexParameteri && p_glTexImage2D &&
           p_glTexSubImage2D && p_glGenTextures && p_glDeleteTextures &&
           p_glBegin && p_glEnd && p_glTexCoord2f && p_glVertex2f &&
           p_glFinish && p_glGetError;
}

bool glx_create(Display *dpy, Window win)
{
    if (g_ready)
        return true;

    g_gl_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_gl_handle) {
        fprintf(stderr, "[glx] cannot load libGL.so.1: %s\n", dlerror());
        return false;
    }

    if (!resolve_glx()) {
        fprintf(stderr, "[glx] GLX functions are incomplete\n");
        return false;
    }

    int ext_eb = 0, ext_err = 0;
    if (!p_glXQueryExtension(dpy, &ext_eb, &ext_err)) {
        fprintf(stderr, "[glx] X server does not advertise the GLX extension\n");
        return false;
    }

    int screen = DefaultScreen(dpy);
    int att[] = {
        GLX_RGBA, GLX_DOUBLEBUFFER,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        None
    };
    XVisualInfo *vi = p_glXChooseVisual(dpy, screen, att);
    if (!vi) {
        /* try without double buffering as a lightweight fallback */
        int att2[] = {
            GLX_RGBA,
            GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
            GLX_DEPTH_SIZE, 24,
            None
        };
        vi = p_glXChooseVisual(dpy, screen, att2);
    }
    if (!vi) {
        fprintf(stderr, "[glx] glXChooseVisual failed\n");
        return false;
    }

    g_ctx = p_glXCreateContext(dpy, vi, NULL, True);
    XFree(vi);
    if (!g_ctx) {
        fprintf(stderr, "[glx] glXCreateContext failed\n");
        return false;
    }

    if (!p_glXMakeCurrent(dpy, (GLXDrawable)win, g_ctx)) {
        fprintf(stderr, "[glx] glXMakeCurrent failed\n");
        p_glXDestroyContext(dpy, g_ctx);
        g_ctx = NULL;
        return false;
    }

    if (!resolve_gl()) {
        fprintf(stderr, "[glx] OpenGL functions are incomplete\n");
        return false;
    }

    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    p_glGenTextures(1, &g_tex);
    p_glBindTexture(GL_TEXTURE_2D, g_tex);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g_dpy = dpy;
    g_win = (GLXDrawable)win;
    g_ready = true;
    printf("[glx] OpenGL composition via GLX is active\n");

    /* Release the context from the main thread before starting the server;
     * glx_lock() makes it current on whichever thread is working. */
    p_glXMakeCurrent(dpy, None, NULL);

    /* Start the streaming server after the context is ready + g_ready. Non-fatal. */
    if (!gles_server_init())
        fprintf(stderr, "[glx] failed to start the GLES streaming server\n");

    return true;
}

void glx_draw(const void *pixels, int width, int height, int stride)
{
    (void)stride; /* texture size is already known; stride is guaranteed by the caller */
    if (!g_ready || !p_glClear)
        return;

    glx_lock();

    if ((size_t)width != (size_t)g_tex_w || (size_t)height != (size_t)g_tex_h) {
        p_glBindTexture(GL_TEXTURE_2D, g_tex);
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height,
                       0, GL_BGRA_EXT, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        g_tex_w = width;
        g_tex_h = height;
    }

    p_glBindTexture(GL_TEXTURE_2D, g_tex);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height,
                      GL_BGRA_EXT, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);

    p_glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    p_glClear(GL_COLOR_BUFFER_BIT);

    p_glMatrixMode(GL_PROJECTION);
    p_glLoadIdentity();
    /* Y grows downward, matching the framebuffer memory layout */
    p_glOrtho(0.0, (GLdouble)width, (GLdouble)height, 0.0, -1.0, 1.0);
    p_glMatrixMode(GL_MODELVIEW);
    p_glLoadIdentity();

    p_glEnable(GL_TEXTURE_2D);
    p_glBindTexture(GL_TEXTURE_2D, g_tex);
    p_glBegin(GL_QUADS);
    p_glTexCoord2f(0.0f, 0.0f); p_glVertex2f(0.0f, 0.0f);
    p_glTexCoord2f(1.0f, 0.0f); p_glVertex2f((GLfloat)width, 0.0f);
    p_glTexCoord2f(1.0f, 1.0f); p_glVertex2f((GLfloat)width, (GLfloat)height);
    p_glTexCoord2f(0.0f, 1.0f); p_glVertex2f(0.0f, (GLfloat)height);
    p_glEnd();
    p_glDisable(GL_TEXTURE_2D);

    p_glFinish();
    p_glXSwapBuffers(g_dpy, g_win);
    glx_unlock();
}

void glx_destroy(void)
{
    if (!g_ready)
        return;

    /* Stop the GLES streaming server before destroying the GLX context */
    gles_server_stop();

    if (g_tex) {
        p_glDeleteTextures(1, &g_tex);
        g_tex = 0;
    }
    if (g_ctx) {
        p_glXMakeCurrent(g_dpy, None, NULL);
        p_glXDestroyContext(g_dpy, g_ctx);
        g_ctx = NULL;
    }
    g_tex_w = g_tex_h = 0;
    g_ready = false;
    if (g_gl_handle) {
        dlclose(g_gl_handle);
        g_gl_handle = NULL;
    }
}

void glx_lock(void)
{
    pthread_mutex_lock(&g_glx_mutex);
    if (g_ready && p_glXMakeCurrent)
        p_glXMakeCurrent(g_dpy, g_win, g_ctx);
}

void glx_unlock(void)
{
    if (g_ready && p_glXMakeCurrent)
        p_glXMakeCurrent(g_dpy, None, NULL);
    pthread_mutex_unlock(&g_glx_mutex);
}

bool glx_ready(void)
{
    return g_ready;
}
