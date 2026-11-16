/* language: C11, file: host-daemon/x11droid_daemon.c, target: Linux Xorg, gcc
 * X11 window 720x1280 + MIT-SHM (XShmCreateImage/XShmPutImage).
 * The framebuffer is received from /dev/shm/x11droid_fb; refresh is triggered
 * by xd_msg messages over the Unix domain socket /tmp/x11droid.sock.
 *
 * build: make -C host-daemon
 * run  : ./build/host-daemon/x11droid-daemon
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fb_proto.h"
#include "glx_render.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XShm.h>

#include "uinput_input.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define WINDOW_W      720
#define WINDOW_H      1280
#define MAX_CLIENTS   8
#define RECV_BUF      256

typedef struct {
    int      fd;
    uint8_t  buf[RECV_BUF];
    size_t   len;
} client_t;

static volatile sig_atomic_t g_stop = 0;

static Display          *g_dpy = NULL;
static Window            g_win = 0;
static GC                g_gc = NULL;
static XImage           *g_img = NULL;
static XShmSegmentInfo   g_shm = {0};
static bool              g_use_xshm = false;
static int               g_shm_opcode = -1;
static bool              g_shm_failed = false;
static xd_fb_header     *g_fb = NULL;
static size_t            g_fb_size = 0;
static uint32_t          g_last_seq = 0;
static bool              g_force_present = true;
static bool              g_want_gl = true;
static bool              g_gl_ready = false;

static xd_input         g_input;
static bool             g_input_ready = false;
static bool             g_dragging = false;
static bool             g_key_down[256];

static int               g_listen_fd = -1;
static char              g_sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)] = XD_SOCK_PATH;
static char              g_shm_name[64] = XD_FB_NAME;
static client_t          g_clients[MAX_CLIENTS];

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("x11droid-daemon: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static int quiet_x_error(Display *dpy, XErrorEvent *ev)
{
    if (g_shm_opcode > 0 && ev->request_code == (unsigned)g_shm_opcode &&
        (ev->error_code == BadAccess || ev->error_code == BadShmSeg))
        g_shm_failed = true;

    char text[128] = {0};
    XGetErrorText(dpy, ev->error_code, text, sizeof text);
    fprintf(stderr, "x11droid-daemon: X error %d (%s) request %d\n",
            ev->error_code, text, ev->request_code);
    return 0;
}

/* ------------------------------------------------------------------ X11 */

static void init_x11(const char *display_name)
{
    g_dpy = XOpenDisplay(display_name);
    if (!g_dpy)
        fatal("cannot open display '%s' (check DISPLAY / that Xorg is running)",
              display_name ? display_name : getenv("DISPLAY"));

    XSetErrorHandler(quiet_x_error);

    const int screen = DefaultScreen(g_dpy);
    const int depth = DefaultDepth(g_dpy, screen);
    if (depth != 24 && depth != 32)
        fatal("visual depth %d is not supported (need 24/32-bit, NVIDIA 470 target)", depth);

    g_win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, screen),
                                0, 0, WINDOW_W, WINDOW_H, 0,
                                BlackPixel(g_dpy, screen), BlackPixel(g_dpy, screen));
    if (!g_win)
        fatal("XCreateSimpleWindow failed");

    XStoreName(g_dpy, g_win, "x11-droid — 720x1280 (MIT-SHM)");

    XSizeHints *hints = XAllocSizeHints();
    if (hints) {
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = WINDOW_W;
        hints->min_height = hints->max_height = WINDOW_H;
        XSetWMNormalHints(g_dpy, g_win, hints);
        XFree(hints);
    }

    Atom wm_delete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &wm_delete, 1);

    g_gc = XCreateGC(g_dpy, g_win, 0, NULL);
    if (!g_gc)
        fatal("XCreateGC failed");

    XSelectInput(g_dpy, g_win, ExposureMask | KeyPressMask | KeyReleaseMask |
                               ButtonPressMask | ButtonReleaseMask |
                               Button1MotionMask | StructureNotifyMask);

    Bool detectable = False;
    XkbSetDetectableAutoRepeat(g_dpy, True, &detectable);
    if (!detectable)
        fprintf(stderr, "[daemon] server has no detectable auto-repeat; "
                        "keyboard repeat may be duplicated\n");
    XMapWindow(g_dpy, g_win);
    XSync(g_dpy, False);
}

/* ---------------------------------------------------------- MIT-SHM image */

static void create_fallback_image(void)
{
    g_img = XCreateImage(g_dpy, DefaultVisual(g_dpy, DefaultScreen(g_dpy)),
                         (unsigned)DefaultDepth(g_dpy, DefaultScreen(g_dpy)),
                         ZPixmap, 0, NULL, WINDOW_W, WINDOW_H, 32, 0);
    if (!g_img)
        fatal("XCreateImage failed");
    g_img->data = calloc((size_t)g_img->bytes_per_line, (size_t)g_img->height);
    if (!g_img->data)
        fatal("calloc XImage failed");
    printf("[daemon] XPutImage fallback (MIT-SHM not used)\n");
}

/* Drop the shm image without XShmDetach: used after the server rejects the segment. */
static void drop_shm_image(void)
{
    if (g_img) {
        g_img->data = NULL; /* shmat, not malloc */
        XDestroyImage(g_img);
        g_img = NULL;
    }
    if (g_shm.shmaddr && g_shm.shmaddr != (char *)-1) {
        shmdt(g_shm.shmaddr);
        g_shm.shmaddr = NULL;
    }
    if (g_shm.shmid >= 0) {
        shmctl(g_shm.shmid, IPC_RMID, NULL);
        g_shm.shmid = -1;
    }
    g_use_xshm = false;
}

static void init_image(void)
{
    int opcode = 0, ev_base = 0, err_base = 0;

    if (g_use_xshm &&
        XQueryExtension(g_dpy, "MIT-SHM", &opcode, &ev_base, &err_base)) {
        g_shm_opcode = opcode;
        g_shm_failed = false;

        g_img = XShmCreateImage(g_dpy, DefaultVisual(g_dpy, DefaultScreen(g_dpy)),
                                (unsigned)DefaultDepth(g_dpy, DefaultScreen(g_dpy)),
                                ZPixmap, NULL, &g_shm, WINDOW_W, WINDOW_H);
        if (g_img) {
            g_shm.shmid = shmget(IPC_PRIVATE,
                                 (size_t)g_img->bytes_per_line * (size_t)g_img->height,
                                 IPC_CREAT | 0600);
            if (g_shm.shmid >= 0) {
                g_shm.shmaddr = shmat(g_shm.shmid, NULL, 0);
                if (g_shm.shmaddr != (char *)-1) {
                    g_shm.readOnly = False;
                    g_img->data = g_shm.shmaddr;

                    const Bool attached = XShmAttach(g_dpy, &g_shm);
                    XSync(g_dpy, False); /* attach errors surface here */

                    if (attached && !g_shm_failed) {
                        /* mark for deletion: the kernel frees it when the process exits */
                        shmctl(g_shm.shmid, IPC_RMID, NULL);
                        g_shm.shmid = -1;
                        printf("[daemon] MIT-SHM active (%dx%d, %d bytes/row)\n",
                               WINDOW_W, WINDOW_H, g_img->bytes_per_line);
                        return;
                    }

                    fprintf(stderr, "[daemon] server rejected XShmAttach%s, "
                                    "XPutImage fallback\n",
                            g_shm_failed ? " (possibly a different X server user)" : "");
                    drop_shm_image();
                    create_fallback_image();
                    return;
                }
                shmctl(g_shm.shmid, IPC_RMID, NULL);
                g_shm.shmid = -1;
            }
            g_img->data = NULL;
            XDestroyImage(g_img);
            g_img = NULL;
        }
        g_use_xshm = false;
    }

    create_fallback_image();
}

/* --------------------------------------------------------------- POSIX shm */

static void init_fb(void)
{
    /* No other daemon is alive, so the stale segment can be removed. */
    shm_unlink(g_shm_name);

    const int fd = shm_open(g_shm_name, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        fatal("shm_open(%s) failed: %s", g_shm_name, strerror(errno));

    g_fb_size = XD_FB_SIZE(WINDOW_W, WINDOW_H);
    if (ftruncate(fd, (off_t)g_fb_size) != 0) {
        close(fd);
        fatal("ftruncate shm failed: %s", strerror(errno));
    }

    void *p = mmap(NULL, g_fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        fatal("mmap shm failed: %s", strerror(errno));

    memset(p, 0, g_fb_size);
    g_fb = p;
    g_fb->version = XD_FB_VERSION;
    g_fb->width = WINDOW_W;
    g_fb->height = WINDOW_H;
    g_fb->stride = WINDOW_W * 4u;
    g_fb->bpp = 4;
    g_fb->pixel_format = XD_PIXEL_BGRA8888;
    atomic_store_explicit(&g_fb->seq, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    g_fb->magic = XD_FB_MAGIC; /* magic is written last */

    printf("[daemon] framebuffer %s (%zu byte, stride %u)\n",
           XD_FB_PATH, g_fb_size, g_fb->stride);
}

/* ---------------------------------------------------------------- socket */

static bool socket_is_live(const char *path)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

    const bool live = connect(fd, (struct sockaddr *)&addr,
                              (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                          strlen(addr.sun_path) + 1)) == 0;
    close(fd);
    return live;
}

static void init_socket(void)
{
    if (socket_is_live(g_sock_path))
        fatal("another daemon already holds %s", g_sock_path);

    unlink(g_sock_path);

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (g_listen_fd < 0)
        fatal("socket(AF_UNIX) failed: %s", strerror(errno));

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    if (snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_sock_path) >=
        (int)sizeof addr.sun_path)
        fatal("socket path too long: %s", g_sock_path);

    const socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                      strlen(addr.sun_path) + 1);
    if (bind(g_listen_fd, (struct sockaddr *)&addr, len) != 0)
        fatal("bind(%s) failed: %s", g_sock_path, strerror(errno));
    if (chmod(g_sock_path, 0600) != 0)
        fprintf(stderr, "[daemon] chmod socket failed: %s\n", strerror(errno));
    if (listen(g_listen_fd, MAX_CLIENTS) != 0)
        fatal("listen(%s) failed: %s", g_sock_path, strerror(errno));

    printf("[daemon] socket %s ready\n", g_sock_path);
}

static void close_client(client_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->len = 0;
}

static void accept_clients(void)
{
    for (;;) {
        const int fd = accept4(g_listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                return;
            fprintf(stderr, "[daemon] accept failed: %s\n", strerror(errno));
            return;
        }

        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (g_clients[i].fd < 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            fprintf(stderr, "[daemon] client slots full, connection rejected\n");
            close(fd);
            continue;
        }
        g_clients[slot].fd = fd;
        g_clients[slot].len = 0;
    }
}

static void drain_client(client_t *c, bool *pending)
{
    for (;;) {
        const ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            close_client(c);
            return;
        }
        if (n == 0) {
            close_client(c);
            return;
        }
        c->len += (size_t)n;

        while (c->len >= sizeof(xd_msg)) {
            xd_msg msg;
            memcpy(&msg, c->buf, sizeof msg);
            if (msg.magic == XD_MSG_MAGIC && msg.seq > g_last_seq)
                *pending = true;
            memmove(c->buf, c->buf + sizeof msg, c->len - sizeof msg);
            c->len -= sizeof msg;
        }

        if (c->len == sizeof c->buf) {
            fprintf(stderr, "[daemon] client buffer full, connection closed\n");
            close_client(c);
            return;
        }
    }
}

/* ---------------------------------------------------------------- render */

static void copy_fb_to_image(void)
{
    const size_t src_stride = g_fb->stride;
    const size_t dst_stride = (size_t)g_img->bytes_per_line;
    const size_t row_bytes = src_stride < dst_stride ? src_stride : dst_stride;
    const uint8_t *src = XD_FB_PIXELS(g_fb);

    for (uint32_t y = 0; y < WINDOW_H; ++y)
        memcpy(g_img->data + (size_t)y * dst_stride,
               src + (size_t)y * src_stride, row_bytes);
}

static bool snapshot_fb(uint32_t *out_seq)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = atomic_load_explicit(&g_fb->seq, memory_order_acquire);
        if (s1 == 0)
            return false; /* producer has not written a frame yet */
        copy_fb_to_image();
        const uint32_t s2 = atomic_load_explicit(&g_fb->seq, memory_order_acquire);
        if (s1 == s2) {
            *out_seq = s1;
            return true;
        }
    }
    copy_fb_to_image();
    *out_seq = atomic_load_explicit(&g_fb->seq, memory_order_acquire);
    return true;
}

/* Read the framebuffer seq without copying pixels: used by the GLX path
 * that reads shared memory directly through an OpenGL texture. */
static bool fb_seq_stable(uint32_t *out_seq)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = atomic_load_explicit(&g_fb->seq, memory_order_acquire);
        if (s1 == 0)
            return false; /* producer has not written a frame yet */
        const uint32_t s2 = atomic_load_explicit(&g_fb->seq, memory_order_acquire);
        if (s1 == s2) {
            *out_seq = s1;
            return true;
        }
    }
    *out_seq = atomic_load_explicit(&g_fb->seq, memory_order_acquire);
    return true;
}

static void present(void)
{
    if (g_gl_ready) {
        /* GLX reads pixels directly from shared memory; there is no CPU copy
         * to the screen, and the NVIDIA 470 GPU handles composition. */
        glx_draw(XD_FB_PIXELS(g_fb), WINDOW_W, WINDOW_H, (int)g_fb->stride);
    } else if (g_use_xshm) {
        XShmPutImage(g_dpy, g_win, g_gc, g_img, 0, 0, 0, 0,
                     WINDOW_W, WINDOW_H, False);
    } else {
        XPutImage(g_dpy, g_win, g_gc, g_img, 0, 0, 0, 0,
                  WINDOW_W, WINDOW_H);
    }
    /* XSync ensures the render request is actually executed by the server
     * before the next frame; this removes origin artifacts under Xvfb. */
    XSync(g_dpy, False);
}

static void render(void)
{
    /* the server may revoke the segment at any time; switch to the copy path */
    if (g_use_xshm && g_shm_failed) {
        fprintf(stderr, "[daemon] MIT-SHM failed at runtime, switching to XPutImage\n");
        g_shm_failed = false;
        drop_shm_image();
        create_fallback_image();
    }

    uint32_t seq = 0;
    if (!g_gl_ready) {
        if (!snapshot_fb(&seq))
            seq = 0;
    } else {
        if (!fb_seq_stable(&seq))
            seq = 0;
    }

    if (!g_force_present && seq == g_last_seq)
        return;

    present();
    g_force_present = false;
    g_last_seq = seq;
}

/* ------------------------------------------------------------------ loop */

static void handle_events(void)
{
    while (XPending(g_dpy) > 0) {
        XEvent ev;
        XNextEvent(g_dpy, &ev);
        switch (ev.type) {
        case Expose:
            if (ev.xexpose.count == 0)
                g_force_present = true;
            break;
        case ButtonPress:
            if (ev.xbutton.button == Button1) {
                g_dragging = true;
                if (g_input_ready)
                    xd_input_touch_down(&g_input, ev.xbutton.x, ev.xbutton.y);
            }
            break;
        case MotionNotify:
            if (g_dragging && g_input_ready)
                xd_input_touch_move(&g_input, ev.xmotion.x, ev.xmotion.y);
            break;
        case ButtonRelease:
            if (ev.xbutton.button == Button1 && g_dragging) {
                g_dragging = false;
                if (g_input_ready)
                    xd_input_touch_up(&g_input);
            }
            break;
        case KeyPress: {
            const unsigned kc = ev.xkey.keycode & 0xffu;
            const KeySym ks = XkbKeycodeToKeysym(g_dpy, ev.xkey.keycode, 0, 0);
            if (ks == XK_Escape) {
                g_stop = 1;
                break;
            }
            if (g_key_down[kc])
                break; /* X auto-repeat, not a new keydown */
            g_key_down[kc] = true;
            if (g_input_ready)
                xd_input_key(&g_input, xd_input_code_from_keysym((unsigned long)ks), true);
            break;
        }
        case KeyRelease: {
            const unsigned kc = ev.xkey.keycode & 0xffu;
            if (!g_key_down[kc])
                break;
            g_key_down[kc] = false;
            if (g_input_ready) {
                const KeySym ks = XkbKeycodeToKeysym(g_dpy, ev.xkey.keycode, 0, 0);
                xd_input_key(&g_input, xd_input_code_from_keysym((unsigned long)ks), false);
            }
            break;
        }
        case ClientMessage:
            /* WM_DELETE_WINDOW */
            g_stop = 1;
            break;
        case DestroyNotify:
            g_stop = 1;
            break;
        default:
            break;
        }
    }

    if (g_input_ready)
        xd_input_touch_flush(&g_input);
}

static void run_loop(void)
{
    while (!g_stop) {
        handle_events();
        if (g_stop)
            break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ConnectionNumber(g_dpy), &rfds);
        FD_SET(g_listen_fd, &rfds);
        int maxfd = g_listen_fd > ConnectionNumber(g_dpy)
                        ? g_listen_fd : ConnectionNumber(g_dpy);

        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (g_clients[i].fd >= 0) {
                FD_SET(g_clients[i].fd, &rfds);
                if (g_clients[i].fd > maxfd)
                    maxfd = g_clients[i].fd;
            }
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        const int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            fatal("select failed: %s", strerror(errno));
        }

        bool pending = false;
        if (ready > 0) {
            if (FD_ISSET(g_listen_fd, &rfds))
                accept_clients();
            for (int i = 0; i < MAX_CLIENTS; ++i) {
                if (g_clients[i].fd >= 0 && FD_ISSET(g_clients[i].fd, &rfds))
                    drain_client(&g_clients[i], &pending);
            }
        }

        if (pending)
            render();
        else if (g_force_present)
            render();
    }
}

/* --------------------------------------------------------------- cleanup */

static void cleanup(void)
{
    for (int i = 0; i < MAX_CLIENTS; ++i)
        close_client(&g_clients[i]);

    if (g_input_ready) {
        xd_input_close(&g_input);
        g_input_ready = false;
    }

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_sock_path[0])
        unlink(g_sock_path);

    if (g_fb) {
        munmap(g_fb, g_fb_size);
        g_fb = NULL;
    }
    shm_unlink(g_shm_name);

    if (g_dpy) {
        if (g_img && g_use_xshm) {
            XShmDetach(g_dpy, &g_shm);
            XSync(g_dpy, False);
        }
        if (g_gc)
            XFreeGC(g_dpy, g_gc);
        if (g_win)
            XDestroyWindow(g_dpy, g_win);
        if (g_img) {
            if (g_use_xshm) {
                g_img->data = NULL; /* allocated via shmat, not malloc */
                if (g_shm.shmaddr && g_shm.shmaddr != (char *)-1)
                    shmdt(g_shm.shmaddr);
            }
            XDestroyImage(g_img);
        }
        XCloseDisplay(g_dpy);
    }

    if (g_gl_ready)
        glx_destroy();
}

/* ------------------------------------------------------------------ main */

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "  --display NAME   X11 display (default: $DISPLAY)\n"
           "  --socket PATH    Unix socket (default: %s)\n"
           "  --shm NAME       POSIX shm name (default: %s)\n"
           "  --no-shm         force XPutImage, skip MIT-SHM\n"
           "  --no-gl          force X11 CPU (MIT-SHM/XPutImage), skip GLX\n"
           "  --no-input       skip creating x11droid-touch/-kbd\n"
           "  -h, --help       show this help\n",
           argv0, XD_SOCK_PATH, XD_FB_NAME);
}

int main(int argc, char **argv)
{
    const char *display_name = NULL;
    bool want_shm = true;
    bool want_input = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            display_name = argv[++i];
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            snprintf(g_sock_path, sizeof g_sock_path, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
            snprintf(g_shm_name, sizeof g_shm_name, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--no-shm") == 0) {
            want_shm = false;
        } else if (strcmp(argv[i], "--no-input") == 0) {
            want_input = false;
        } else if (strcmp(argv[i], "--no-gl") == 0) {
            g_want_gl = false;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    for (int i = 0; i < MAX_CLIENTS; ++i)
        g_clients[i].fd = -1;

    g_use_xshm = want_shm;

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    init_fb();
    init_x11(display_name);
    init_image();

    if (g_want_gl)
        g_gl_ready = glx_create(g_dpy, g_win);

    if (want_input) {
        char err[256];
        if (xd_input_init(&g_input, WINDOW_W - 1, WINDOW_H - 1,
                          err, sizeof err) == 0) {
            g_input_ready = true;
            printf("[daemon] virtual input: x11droid-touch (abs 0..%d x 0..%d), "
                   "x11droid-kbd\n", WINDOW_W - 1, WINDOW_H - 1);
        } else {
            fprintf(stderr, "[daemon] virtual input failed: %s "
                            "(continuing without input)\n", err);
        }
    }

    init_socket();

    printf("[daemon] window %dx%d open — press Esc to quit\n", WINDOW_W, WINDOW_H);
    g_force_present = true;
    run_loop();
    printf("[daemon] exiting, cleaning up\n");

    cleanup();
    return 0;
}
