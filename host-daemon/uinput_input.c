/* language: C11, file: host-daemon/uinput_input.c, target: Linux uinput
 * Creates an absolute /dev/uinput touchscreen and keyboard, then writes
 * input_event records to both.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "uinput_input.h"

#include <X11/keysym.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define XD_TOUCH_NAME "x11droid-touch"
#define XD_KBD_NAME   "x11droid-kbd"

typedef struct {
    unsigned long keysym;
    uint16_t      code;
} keymap_entry;

static const keymap_entry g_keymap[] = {
    { XK_a, KEY_A }, { XK_b, KEY_B }, { XK_c, KEY_C }, { XK_d, KEY_D },
    { XK_e, KEY_E }, { XK_f, KEY_F }, { XK_g, KEY_G }, { XK_h, KEY_H },
    { XK_i, KEY_I }, { XK_j, KEY_J }, { XK_k, KEY_K }, { XK_l, KEY_L },
    { XK_m, KEY_M }, { XK_n, KEY_N }, { XK_o, KEY_O }, { XK_p, KEY_P },
    { XK_q, KEY_Q }, { XK_r, KEY_R }, { XK_s, KEY_S }, { XK_t, KEY_T },
    { XK_u, KEY_U }, { XK_v, KEY_V }, { XK_w, KEY_W }, { XK_x, KEY_X },
    { XK_y, KEY_Y }, { XK_z, KEY_Z },
    { XK_A, KEY_A }, { XK_B, KEY_B }, { XK_C, KEY_C }, { XK_D, KEY_D },
    { XK_E, KEY_E }, { XK_F, KEY_F }, { XK_G, KEY_G }, { XK_H, KEY_H },
    { XK_I, KEY_I }, { XK_J, KEY_J }, { XK_K, KEY_K }, { XK_L, KEY_L },
    { XK_M, KEY_M }, { XK_N, KEY_N }, { XK_O, KEY_O }, { XK_P, KEY_P },
    { XK_Q, KEY_Q }, { XK_R, KEY_R }, { XK_S, KEY_S }, { XK_T, KEY_T },
    { XK_U, KEY_U }, { XK_V, KEY_V }, { XK_W, KEY_W }, { XK_X, KEY_X },
    { XK_Y, KEY_Y }, { XK_Z, KEY_Z },

    { XK_0, KEY_0 }, { XK_1, KEY_1 }, { XK_2, KEY_2 }, { XK_3, KEY_3 },
    { XK_4, KEY_4 }, { XK_5, KEY_5 }, { XK_6, KEY_6 }, { XK_7, KEY_7 },
    { XK_8, KEY_8 }, { XK_9, KEY_9 },

    { XK_minus, KEY_MINUS }, { XK_equal, KEY_EQUAL },
    { XK_bracketleft, KEY_LEFTBRACE }, { XK_bracketright, KEY_RIGHTBRACE },
    { XK_backslash, KEY_BACKSLASH }, { XK_semicolon, KEY_SEMICOLON },
    { XK_apostrophe, KEY_APOSTROPHE }, { XK_grave, KEY_GRAVE },
    { XK_comma, KEY_COMMA }, { XK_period, KEY_DOT }, { XK_slash, KEY_SLASH },

    { XK_BackSpace, KEY_BACKSPACE }, { XK_Tab, KEY_TAB },
    { XK_Return, KEY_ENTER }, { XK_Escape, KEY_ESC }, { XK_space, KEY_SPACE },
    { XK_Delete, KEY_DELETE }, { XK_Insert, KEY_INSERT },
    { XK_Home, KEY_HOME }, { XK_End, KEY_END },
    { XK_Page_Up, KEY_PAGEUP }, { XK_Page_Down, KEY_PAGEDOWN },
    { XK_Left, KEY_LEFT }, { XK_Right, KEY_RIGHT },
    { XK_Up, KEY_UP }, { XK_Down, KEY_DOWN },

    { XK_Shift_L, KEY_LEFTSHIFT }, { XK_Shift_R, KEY_RIGHTSHIFT },
    { XK_Control_L, KEY_LEFTCTRL }, { XK_Control_R, KEY_RIGHTCTRL },
    { XK_Alt_L, KEY_LEFTALT }, { XK_Alt_R, KEY_RIGHTALT },
    { XK_Super_L, KEY_LEFTMETA }, { XK_Super_R, KEY_RIGHTMETA },
    { XK_Caps_Lock, KEY_CAPSLOCK }, { XK_Num_Lock, KEY_NUMLOCK },
    { XK_Scroll_Lock, KEY_SCROLLLOCK },

    { XK_F1, KEY_F1 }, { XK_F2, KEY_F2 }, { XK_F3, KEY_F3 }, { XK_F4, KEY_F4 },
    { XK_F5, KEY_F5 }, { XK_F6, KEY_F6 }, { XK_F7, KEY_F7 }, { XK_F8, KEY_F8 },
    { XK_F9, KEY_F9 }, { XK_F10, KEY_F10 }, { XK_F11, KEY_F11 }, { XK_F12, KEY_F12 },

    { XK_KP_0, KEY_KP0 }, { XK_KP_1, KEY_KP1 }, { XK_KP_2, KEY_KP2 },
    { XK_KP_3, KEY_KP3 }, { XK_KP_4, KEY_KP4 }, { XK_KP_5, KEY_KP5 },
    { XK_KP_6, KEY_KP6 }, { XK_KP_7, KEY_KP7 }, { XK_KP_8, KEY_KP8 },
    { XK_KP_9, KEY_KP9 }, { XK_KP_Enter, KEY_KPENTER },
    { XK_KP_Add, KEY_KPPLUS }, { XK_KP_Subtract, KEY_KPMINUS },
    { XK_KP_Multiply, KEY_KPASTERISK }, { XK_KP_Divide, KEY_KPSLASH },
    { XK_KP_Decimal, KEY_KPDOT },

    { XK_Print, KEY_SYSRQ }, { XK_Menu, KEY_MENU }, { XK_Pause, KEY_PAUSE },
};

uint16_t xd_input_code_from_keysym(unsigned long keysym)
{
    for (size_t i = 0; i < sizeof g_keymap / sizeof g_keymap[0]; ++i) {
        if (g_keymap[i].keysym == keysym)
            return g_keymap[i].code;
    }
    return 0;
}

static int emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = value;

    ssize_t n;
    do {
        n = write(fd, &ev, sizeof ev);
    } while (n < 0 && errno == EINTR);

    return n == (ssize_t)sizeof ev ? 0 : -1;
}

static int clamp_axis(int v, uint32_t max)
{
    if (v < 0)
        return 0;
    if ((uint32_t)v > max)
        return (int)max;
    return v;
}

static int set_keybits(int fd, const uint16_t *codes, size_t count,
                       char *err, size_t errlen)
{
    for (size_t i = 0; i < count; ++i) {
        if (ioctl(fd, UI_SET_KEYBIT, codes[i]) < 0) {
            snprintf(err, errlen, "UI_SET_KEYBIT 0x%x: %s", codes[i], strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int set_absbit(int fd, uint16_t code, int32_t min, int32_t max,
                      char *err, size_t errlen)
{
    if (ioctl(fd, UI_SET_ABSBIT, code) < 0) {
        snprintf(err, errlen, "UI_SET_ABSBIT 0x%x: %s", code, strerror(errno));
        return -1;
    }
    struct uinput_abs_setup abs = {0};
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.resolution = 1;
    if (ioctl(fd, UI_ABS_SETUP, &abs) < 0) {
        snprintf(err, errlen, "UI_ABS_SETUP 0x%x: %s", code, strerror(errno));
        return -1;
    }
    return 0;
}

static int touch_create(xd_input *in, char *err, size_t errlen)
{
    const int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errlen, "open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    const uint16_t keys[] = { BTN_TOUCH, BTN_TOOL_FINGER };
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        set_keybits(fd, keys, sizeof keys / sizeof keys[0], err, errlen) != 0 ||
        set_absbit(fd, ABS_MT_SLOT, 0, 0, err, errlen) != 0 ||
        set_absbit(fd, ABS_MT_TRACKING_ID, -1, 65535, err, errlen) != 0 ||
        set_absbit(fd, ABS_MT_POSITION_X, 0, (int32_t)in->max_x, err, errlen) != 0 ||
        set_absbit(fd, ABS_MT_POSITION_Y, 0, (int32_t)in->max_y, err, errlen) != 0 ||
        set_absbit(fd, ABS_X, 0, (int32_t)in->max_x, err, errlen) != 0 ||
        set_absbit(fd, ABS_Y, 0, (int32_t)in->max_y, err, errlen) != 0) {
        close(fd);
        return -1;
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof setup);
    snprintf(setup.name, sizeof setup.name, "%s", XD_TOUCH_NAME);
    setup.id = (struct input_id){ .bustype = BUS_VIRTUAL,
                                  .vendor = 0x1209, .product = 0x0001,
                                  .version = 1 };

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        snprintf(err, errlen, "UI_DEV_SETUP/CREATE touch: %s", strerror(errno));
        close(fd);
        return -1;
    }

    in->touch_fd = fd;
    return 0;
}

static int kbd_create(xd_input *in, char *err, size_t errlen)
{
    const int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errlen, "open /dev/uinput: %s", strerror(errno));
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        snprintf(err, errlen, "UI_SET_EVBIT kbd: %s", strerror(errno));
        close(fd);
        return -1;
    }

    for (size_t i = 0; i < sizeof g_keymap / sizeof g_keymap[0]; ++i) {
        if (ioctl(fd, UI_SET_KEYBIT, g_keymap[i].code) < 0) {
            snprintf(err, errlen, "UI_SET_KEYBIT 0x%x: %s",
                     g_keymap[i].code, strerror(errno));
            close(fd);
            return -1;
        }
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof setup);
    snprintf(setup.name, sizeof setup.name, "%s", XD_KBD_NAME);
    setup.id = (struct input_id){ .bustype = BUS_VIRTUAL,
                                  .vendor = 0x1209, .product = 0x0002,
                                  .version = 1 };

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(fd, UI_DEV_CREATE) < 0) {
        snprintf(err, errlen, "UI_DEV_SETUP/CREATE kbd: %s", strerror(errno));
        close(fd);
        return -1;
    }

    in->kbd_fd = fd;
    return 0;
}

int xd_input_init(xd_input *in, uint32_t max_x, uint32_t max_y,
                  char *err, size_t errlen)
{
    memset(in, 0, sizeof *in);
    in->touch_fd = -1;
    in->kbd_fd = -1;
    in->max_x = max_x;
    in->max_y = max_y;
    in->next_tracking_id = 1;

    if (err && errlen)
        err[0] = '\0';

    if (touch_create(in, err, errlen) != 0)
        return -1;

    if (kbd_create(in, err, errlen) != 0) {
        ioctl(in->touch_fd, UI_DEV_DESTROY);
        close(in->touch_fd);
        in->touch_fd = -1;
        return -1;
    }

    /* give udev time to expose the event node */
    usleep(50000);
    return 0;
}

void xd_input_touch_down(xd_input *in, int x, int y)
{
    if (in->touch_fd < 0)
        return;

    const int cx = clamp_axis(x, in->max_x);
    const int cy = clamp_axis(y, in->max_y);

    emit(in->touch_fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(in->touch_fd, EV_ABS, ABS_MT_TRACKING_ID, (int32_t)in->next_tracking_id);
    in->next_tracking_id = in->next_tracking_id % 65000u + 1u;

    emit(in->touch_fd, EV_ABS, ABS_MT_POSITION_X, cx);
    emit(in->touch_fd, EV_ABS, ABS_MT_POSITION_Y, cy);
    emit(in->touch_fd, EV_ABS, ABS_X, cx);
    emit(in->touch_fd, EV_ABS, ABS_Y, cy);
    emit(in->touch_fd, EV_KEY, BTN_TOOL_FINGER, 1);
    emit(in->touch_fd, EV_KEY, BTN_TOUCH, 1);
    emit(in->touch_fd, EV_SYN, SYN_REPORT, 0);

    in->touch_pending = false;
}

void xd_input_touch_move(xd_input *in, int x, int y)
{
    if (in->touch_fd < 0)
        return;

    const int cx = clamp_axis(x, in->max_x);
    const int cy = clamp_axis(y, in->max_y);

    emit(in->touch_fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(in->touch_fd, EV_ABS, ABS_MT_POSITION_X, cx);
    emit(in->touch_fd, EV_ABS, ABS_MT_POSITION_Y, cy);
    emit(in->touch_fd, EV_ABS, ABS_X, cx);
    emit(in->touch_fd, EV_ABS, ABS_Y, cy);
    in->touch_pending = true;
}

void xd_input_touch_flush(xd_input *in)
{
    if (in->touch_fd < 0 || !in->touch_pending)
        return;
    emit(in->touch_fd, EV_SYN, SYN_REPORT, 0);
    in->touch_pending = false;
}

void xd_input_touch_up(xd_input *in)
{
    if (in->touch_fd < 0)
        return;

    emit(in->touch_fd, EV_ABS, ABS_MT_SLOT, 0);
    emit(in->touch_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(in->touch_fd, EV_KEY, BTN_TOOL_FINGER, 0);
    emit(in->touch_fd, EV_KEY, BTN_TOUCH, 0);
    emit(in->touch_fd, EV_SYN, SYN_REPORT, 0);

    in->touch_pending = false;
}

void xd_input_key(xd_input *in, uint16_t evdev_code, bool down)
{
    if (in->kbd_fd < 0 || evdev_code == 0)
        return;
    emit(in->kbd_fd, EV_KEY, evdev_code, down ? 1 : 0);
    emit(in->kbd_fd, EV_SYN, SYN_REPORT, 0);
}

void xd_input_close(xd_input *in)
{
    if (in->touch_fd >= 0) {
        ioctl(in->touch_fd, UI_DEV_DESTROY);
        close(in->touch_fd);
        in->touch_fd = -1;
    }
    if (in->kbd_fd >= 0) {
        ioctl(in->kbd_fd, UI_DEV_DESTROY);
        close(in->kbd_fd);
        in->kbd_fd = -1;
    }
}
