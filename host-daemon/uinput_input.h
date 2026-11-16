/* language: C11, file: host-daemon/uinput_input.h, target: Linux uinput
 * Two virtual devices:
 *   x11droid-touch — absolute multitouch (protocol B, single slot)
 *   x11droid-kbd   — EV_KEY keyboard
 */
#ifndef XD_UINPUT_INPUT_H
#define XD_UINPUT_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int      touch_fd;
    int      kbd_fd;
    uint32_t max_x;
    uint32_t max_y;
    uint32_t next_tracking_id;
    bool     touch_pending; /* a movement is waiting for SYN_REPORT */
} xd_input;

/* returns 0 on success; -1 and a message in err on failure */
int xd_input_init(xd_input *in, uint32_t max_x, uint32_t max_y,
                  char *err, size_t errlen);
void xd_input_touch_down(xd_input *in, int x, int y);
void xd_input_touch_move(xd_input *in, int x, int y); /* without SYN; flushed later */
void xd_input_touch_flush(xd_input *in);
void xd_input_touch_up(xd_input *in);
void xd_input_key(xd_input *in, uint16_t evdev_code, bool down);
void xd_input_close(xd_input *in);

/* X11 KeySym -> EV_KEY code; 0 when unmapped */
uint16_t xd_input_code_from_keysym(unsigned long keysym);

#endif /* XD_UINPUT_INPUT_H */
