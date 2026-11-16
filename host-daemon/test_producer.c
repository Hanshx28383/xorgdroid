/* language: C11, file: host-daemon/test_producer.c, target: Linux, gcc
 * Dummy producer: writes a moving color pattern to /dev/shm/x11droid_fb,
 * then sends xd_msg to /tmp/x11droid.sock to trigger an X11 refresh.
 *
 * build: make -C host-daemon
 * run  : ./build/host-daemon/test_producer --fps 30
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fb_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t deadline_ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

static int connect_retry(const char *path, int attempts)
{
    for (int i = 0; i < attempts; ++i) {
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return -1;

        struct sockaddr_un addr = {0};
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);

        if (connect(fd, (struct sockaddr *)&addr,
                    (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                strlen(addr.sun_path) + 1)) == 0)
            return fd;

        close(fd);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
        nanosleep(&ts, NULL);
    }
    return -1;
}

static int send_message(int fd, uint32_t seq)
{
    const xd_msg msg = { .magic = XD_MSG_MAGIC, .seq = seq };
    const uint8_t *p = (const uint8_t *)&msg;
    size_t left = sizeof msg;

    while (left > 0) {
        const ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

/* Pattern: moving concentric rings plus a bright diagonal sweep. */
static void fill_frame(uint32_t *pixels, uint32_t w, uint32_t h,
                       uint32_t stride_px, uint32_t frame)
{
    const int32_t cx = (int32_t)(w / 2);
    const int32_t cy = (int32_t)(h / 2);
    const uint32_t phase = frame * 6u;

    for (uint32_t y = 0; y < h; ++y) {
        uint32_t *row = pixels + (size_t)y * stride_px;
        const int32_t dy = (int32_t)y - cy;
        for (uint32_t x = 0; x < w; ++x) {
            const int32_t dx = (int32_t)x - cx;
            const uint32_t d = (uint32_t)((dx * dx + dy * dy) >> 8);
            const uint32_t v = (d + phase) & 0x1FFu;

            uint8_t r = (uint8_t)(v < 256u ? v : 511u - v);
            uint8_t g = (uint8_t)((v * 3u + 85u) & 0xFFu);
            uint8_t b = (uint8_t)((v * 5u + 170u) & 0xFFu);

            if (((x + y + frame * 4u) & 0x3Fu) < 3u) {
                r = 255;
                g = 255;
                b = 255;
            }
            row[x] = ((uint32_t)0xFFu << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "  --fps N         frames per second (default: 30)\n"
           "  --frames N      number of frames before exit; 0 = run forever (default: 0)\n"
           "  --socket PATH   daemon Unix socket (default: %s)\n"
           "  --shm NAME      POSIX shm name (default: %s)\n"
           "  -h, --help      show this help\n",
           argv0, XD_SOCK_PATH, XD_FB_NAME);
}

int main(int argc, char **argv)
{
    unsigned fps = 30;
    uint64_t max_frames = 0;
    const char *socket_path = XD_SOCK_PATH;
    const char *shm_name = XD_FB_NAME;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
            shm_name = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (fps == 0 || fps > 240) {
        fprintf(stderr, "test_producer: --fps must be 1..240\n");
        return 2;
    }

    const int fd = shm_open(shm_name, O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr, "test_producer: shm_open(%s) failed: %s "
                        "(start x11droid-daemon first)\n", shm_name, strerror(errno));
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "test_producer: fstat failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    void *base = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        fprintf(stderr, "test_producer: mmap failed: %s\n", strerror(errno));
        return 1;
    }

    xd_fb_header *hdr = base;
    if (hdr->magic != XD_FB_MAGIC || hdr->version != XD_FB_VERSION) {
        fprintf(stderr, "test_producer: invalid shm header "
                        "(magic 0x%08x version %u)\n", hdr->magic, hdr->version);
        munmap(base, (size_t)st.st_size);
        return 1;
    }
    if (hdr->bpp != 4 || hdr->width == 0 || hdr->height == 0 ||
        hdr->stride < hdr->width * 4u) {
        fprintf(stderr, "test_producer: unsupported geometry "
                        "(%ux%u, bpp %u, stride %u)\n",
                hdr->width, hdr->height, hdr->bpp, hdr->stride);
        munmap(base, (size_t)st.st_size);
        return 1;
    }

    const size_t need = XD_FB_SIZE(hdr->width, hdr->height);
    if ((size_t)st.st_size < need) {
        fprintf(stderr, "test_producer: shm segment is too small (%zu < %zu)\n",
                (size_t)st.st_size, need);
        munmap(base, (size_t)st.st_size);
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int sock = connect_retry(socket_path, 30);
    if (sock < 0) {
        fprintf(stderr, "test_producer: cannot connect to %s\n", socket_path);
        munmap(base, (size_t)st.st_size);
        return 1;
    }

    printf("[producer] %ux%u stride %u — %u fps -> %s\n",
           hdr->width, hdr->height, hdr->stride, fps, XD_FB_PATH);

    uint32_t *pixels = (uint32_t *)XD_FB_PIXELS(hdr);
    const uint32_t stride_px = hdr->stride / 4u;
    const uint64_t period = 1000000000ull / fps;
    uint64_t next = now_ns();
    uint64_t frames = 0;

    while (!g_stop && (max_frames == 0 || frames < max_frames)) {
        const uint32_t seq = (uint32_t)(frames + 1);
        fill_frame(pixels, hdr->width, hdr->height, stride_px, (uint32_t)frames);
        atomic_store_explicit(&hdr->seq, seq, memory_order_release);

        if (send_message(sock, seq) != 0) {
            close(sock);
            sock = connect_retry(socket_path, 10);
            if (sock < 0) {
                fprintf(stderr, "test_producer: daemon disappeared, stopping\n");
                break;
            }
            if (send_message(sock, seq) != 0)
                break;
        }

        ++frames;
        next += period;
        const uint64_t t = now_ns();
        if (next > t)
            sleep_until(next);
        else
            next = t;
    }

    close(sock);
    munmap(base, (size_t)st.st_size);
    printf("[producer] done, %llu frames\n", (unsigned long long)frames);
    return 0;
}
