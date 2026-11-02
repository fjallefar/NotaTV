/* tvmoon - input: evdev -> the host.
 *
 * Controllers (up to 4) are sent as Xbox-like controllers. Keyboards are sent as Windows VK codes
 * (US layout), mice as relative movement, buttons and wheel. Quit the stream:
 *   controller: Select + Start + LB + RB (like Moonlight)
 *   keyboard:   Ctrl + Alt + Shift + Q
 *   remote (CEC): Red = menu (Resume / Stats / Quit). Green = stats overlay
 *   (holding Back goes to LG's own menu)
 * The remote's arrows/OK/Back are sent as a controller (d-pad/A/B), so you can navigate without a controller.
 * Keys the host cannot use (volume, power, media) are not forwarded.
 */
#define _GNU_SOURCE
#include "tvmoon.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define MAX_DEV 32
#define MAX_PADS 4

volatile bool quit_requested;
volatile bool menu_open, menu_dirty;
volatile int menu_sel, menu_pick = -1;

typedef struct {
    int fd;
    char node[16];
    int pad; /* controller number, -1 = not a controller */
    bool remote; /* HDMI-CEC (vc4-hdmi) */
    bool kbd; /* a keyboard: keys are forwarded to the host as VK codes */
    bool mouse; /* relative pointer: movement, buttons and wheel are forwarded */
    short mdx, mdy; /* movement collected since the last SYN_REPORT */
    signed char wheel, hwheel;
    struct input_absinfo abs[0x40];
    bool has_abs[0x40];
} Dev;

typedef struct {
    bool used;
    int buttons;
    unsigned char lt, rt;
    short lx, ly, rx, ry;
    bool dirty;
} Pad;

static Dev devs[MAX_DEV];
static int n_devs;
static Pad pads[MAX_PADS];
static int ino = -1;
static pthread_t th;
static volatile bool running;
static char k_mods; /* MODIFIER_* currently held on the keyboard */
static int remote_pad = -1; /* controller number for the remote (created on the first press) */

/* Linux keycode -> Windows virtual key code (US layout), indexed by the evdev code.
 * Same table as moonlight-embedded (GPL-3.0+, the license tvmoon already has). 0 = not sent. */
static const short vk_codes[] = {
    0x00, /* KEY_RESERVED */
    0x1B, /* KEY_ESC */
    0x31, /* KEY_1 */
    0x32, /* KEY_2 */
    0x33, /* KEY_3 */
    0x34, /* KEY_4 */
    0x35, /* KEY_5 */
    0x36, /* KEY_6 */
    0x37, /* KEY_7 */
    0x38, /* KEY_8 */
    0x39, /* KEY_9 */
    0x30, /* KEY_0 */
    0xBD, /* KEY_MINUS */
    0xBB, /* KEY_EQUAL */
    0x08, /* KEY_BACKSPACE */
    0x09, /* KEY_TAB */
    0x51, /* KEY_Q */
    0x57, /* KEY_W */
    0x45, /* KEY_E */
    0x52, /* KEY_R */
    0x54, /* KEY_T */
    0x59, /* KEY_Y */
    0x55, /* KEY_U */
    0x49, /* KEY_I */
    0x4F, /* KEY_O */
    0x50, /* KEY_P */
    0xDB, /* KEY_LEFTBRACE */
    0xDD, /* KEY_RIGHTBRACE */
    0x0D, /* KEY_ENTER */
    0xA2, /* KEY_LEFTCTRL */
    0x41, /* KEY_A */
    0x53, /* KEY_S */
    0x44, /* KEY_D */
    0x46, /* KEY_F */
    0x47, /* KEY_G */
    0x48, /* KEY_H */
    0x4A, /* KEY_J */
    0x4B, /* KEY_K */
    0x4C, /* KEY_L */
    0xBA, /* KEY_SEMICOLON */
    0xDE, /* KEY_APOSTROPHE */
    0xC0, /* KEY_GRAVE */
    0xA0, /* KEY_LEFTSHIFT */
    0xDC, /* KEY_BACKSLASH */
    0x5A, /* KEY_Z */
    0x58, /* KEY_X */
    0x43, /* KEY_C */
    0x56, /* KEY_V */
    0x42, /* KEY_B */
    0x4E, /* KEY_N */
    0x4D, /* KEY_M */
    0xBC, /* KEY_COMMA */
    0xBE, /* KEY_DOT */
    0xBF, /* KEY_SLASH */
    0xA1, /* KEY_RIGHTSHIFT */
    0x6A, /* KEY_KPASTERISK */
    0xA4, /* KEY_LEFTALT */
    0x20, /* KEY_SPACE */
    0x14, /* KEY_CAPSLOCK */
    0x70, /* KEY_F1 */
    0x71, /* KEY_F2 */
    0x72, /* KEY_F3 */
    0x73, /* KEY_F4 */
    0x74, /* KEY_F5 */
    0x75, /* KEY_F6 */
    0x76, /* KEY_F7 */
    0x77, /* KEY_F8 */
    0x78, /* KEY_F9 */
    0x79, /* KEY_F10 */
    0x90, /* KEY_NUMLOCK */
    0x91, /* KEY_SCROLLLOCK */
    0x67, /* KEY_KP7 */
    0x68, /* KEY_KP8 */
    0x69, /* KEY_KP9 */
    0x6D, /* KEY_KPMINUS */
    0x64, /* KEY_KP4 */
    0x65, /* KEY_KP5 */
    0x66, /* KEY_KP6 */
    0x6B, /* KEY_KPPLUS */
    0x61, /* KEY_KP1 */
    0x62, /* KEY_KP2 */
    0x63, /* KEY_KP3 */
    0x60, /* KEY_KP0 */
    0x6E, /* KEY_KPDOT */
    0x00,
    0x00, /* KEY_ZENKAKUHANKAKU */
    0x00, /* KEY_102ND */
    0x7A, /* KEY_F11 */
    0x7B, /* KEY_F12 */
    0x00, /* KEY_RO */
    0x00, /* KEY_KATAKANA */
    0x00, /* KEY_HIRAGANA */
    0x00, /* KEY_HENKAN */
    0x00, /* KEY_KATAKANAHIRAGANA */
    0x00, /* KEY_MUHENKAN */
    0x00, /* KEY_KPJPCOMMA */
    0x0D, /* KEY_KPENTER */
    0xA3, /* KEY_RIGHTCTRL */
    0x6F, /* KEY_KPSLASH */
    0x2C, /* KEY_SYSRQ */
    0xA5, /* KEY_RIGHTALT */
    0x00, /* KEY_LINEFEED */
    0x24, /* KEY_HOME */
    0x26, /* KEY_UP */
    0x21, /* KEY_PAGEUP */
    0x25, /* KEY_LEFT */
    0x27, /* KEY_RIGHT */
    0x23, /* KEY_END */
    0x28, /* KEY_DOWN */
    0x22, /* KEY_PAGEDOWN */
    0x2D, /* KEY_INSERT */
    0x2E, /* KEY_DELETE */
    0x00, /* KEY_MACRO */
    0x00, /* KEY_MUTE */
    0x00, /* KEY_VOLUMEDOWN */
    0x00, /* KEY_VOLUMEUP */
    0x00, /* KEY_POWER */
    0x00, /* KEY_KPEQUAL */
    0x00, /* KEY_KPPLUSMINUS */
    0x13, /* KEY_PAUSE */
    0x00, /* KEY_SCALE */
    0x00, /* KEY_KPCOMMA */
    0x00, /* KEY_HANGEUL */
    0x00, /* KEY_HANJA */
    0x00, /* KEY_YEN */
    0x5B, /* KEY_LEFTMETA */
    0x5C, /* KEY_RIGHTMETA */
    0x00, /* KEY_COMPOSE */
};

#define TEST_BIT(arr, b) ((arr[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

static short pad_mask(void) {
    short m = 0;
    for (int i = 0; i < MAX_PADS; i++) if (pads[i].used) m |= (short)(1 << i);
    return m;
}

static void open_dev(const char *name) {
    if (strncmp(name, "event", 5) != 0 || n_devs >= MAX_DEV) return;
    for (int i = 0; i < n_devs; i++) if (!strcmp(devs[i].node, name)) return;
    char path[64];
    snprintf(path, sizeof path, "/dev/input/%s", name);
    int fd = -1;
    for (int t = 0; t < 20 && fd < 0; t++) {
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 && errno == ENOENT) return;
        if (fd < 0) usleep(5000);
    }
    if (fd < 0) return;
    unsigned long keys[KEY_CNT / (8 * sizeof(long)) + 1] = {0};
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    char devname[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof devname), devname);
    Dev *d = &devs[n_devs];
    memset(d, 0, sizeof *d);
    d->fd = fd;
    snprintf(d->node, sizeof d->node, "%s", name);
    d->pad = -1;
    d->remote = !strncmp(devname, "vc4-hdmi", 8) || strcasestr(devname, "cec");
    if (!d->remote) {
        unsigned long rel[(REL_MAX / (8 * sizeof(long))) + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
        d->mouse = TEST_BIT(rel, REL_X) && TEST_BIT(rel, REL_Y);
        d->kbd = TEST_BIT(keys, KEY_A) && TEST_BIT(keys, KEY_Z);
    }
    if (TEST_BIT(keys, BTN_SOUTH) || TEST_BIT(keys, BTN_GAMEPAD)) {
        for (int i = 0; i < MAX_PADS; i++)
            if (!pads[i].used) { d->pad = i; break; }
        if (d->pad >= 0) {
            Pad *p = &pads[d->pad];
            memset(p, 0, sizeof *p);
            p->used = true;
            for (int a = 0; a < 0x40; a++)
                if (ioctl(fd, EVIOCGABS(a), &d->abs[a]) == 0 && d->abs[a].maximum > d->abs[a].minimum) d->has_abs[a] = true;
            uint32_t btns = A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG |
                            RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG | SPECIAL_FLAG;
            LiSendControllerArrivalEvent((uint8_t)d->pad, (uint16_t)pad_mask(), LI_CTYPE_UNKNOWN, btns,
                                         d->has_abs[ABS_Z] ? LI_CCAP_ANALOG_TRIGGERS : 0);
        }
    }
    tlog("input: %s (%s)%s%s%s", path, devname,
         d->pad >= 0 ? " = controller" : "", d->kbd ? " = keyboard" : "", d->mouse ? " = mouse" : "");
    n_devs++;
}

static void close_dev(int i) {
    if (devs[i].pad >= 0) {
        pads[devs[i].pad].used = false;
        LiSendMultiControllerEvent((short)devs[i].pad, pad_mask(), 0, 0, 0, 0, 0, 0, 0);
    }
    close(devs[i].fd);
    devs[i] = devs[--n_devs];
}

/* axis -> -32768..32767 (Y is flipped: the host expects up = positive) */
static short norm_axis(const struct input_absinfo *a, int v, bool invert) {
    double c = (a->minimum + a->maximum) / 2.0, h = (a->maximum - a->minimum) / 2.0;
    double x = h > 0 ? (v - c) / h : 0;
    if (invert) x = -x;
    if (x > 1) x = 1;
    if (x < -1) x = -1;
    return (short)(x * 32767);
}

static unsigned char norm_trigger(const struct input_absinfo *a, int v) {
    double r = a->maximum > a->minimum ? (double)(v - a->minimum) / (a->maximum - a->minimum) : 0;
    return (unsigned char)(r < 0 ? 0 : r > 1 ? 255 : r * 255);
}

static void pad_event(Dev *d, const struct input_event *e) {
    Pad *p = &pads[d->pad];
    if (e->type == EV_KEY) {
        int f = 0;
        switch (e->code) {
        case BTN_SOUTH: f = A_FLAG; break;
        case BTN_EAST: f = B_FLAG; break;
        case BTN_NORTH: f = Y_FLAG; break; /* Linux' gamepad standard: NORTH = the top button (Xbox Y) */
        case BTN_WEST: f = X_FLAG; break;
        case BTN_TL: f = LB_FLAG; break;
        case BTN_TR: f = RB_FLAG; break;
        case BTN_SELECT: f = BACK_FLAG; break;
        case BTN_START: f = PLAY_FLAG; break;
        case BTN_MODE: f = SPECIAL_FLAG; break;
        case BTN_THUMBL: f = LS_CLK_FLAG; break;
        case BTN_THUMBR: f = RS_CLK_FLAG; break;
        case BTN_DPAD_UP: f = UP_FLAG; break;
        case BTN_DPAD_DOWN: f = DOWN_FLAG; break;
        case BTN_DPAD_LEFT: f = LEFT_FLAG; break;
        case BTN_DPAD_RIGHT: f = RIGHT_FLAG; break;
        case BTN_TL2: if (!d->has_abs[ABS_Z]) { p->lt = e->value ? 255 : 0; p->dirty = true; } return;
        case BTN_TR2: if (!d->has_abs[ABS_RZ]) { p->rt = e->value ? 255 : 0; p->dirty = true; } return;
        default: return;
        }
        if (e->value) p->buttons |= f;
        else p->buttons &= ~f;
        p->dirty = true;
        int combo = BACK_FLAG | PLAY_FLAG | LB_FLAG | RB_FLAG;
        if ((p->buttons & combo) == combo) { tlog("input: afslut-kombination (controller)"); quit_requested = true; }
    } else if (e->type == EV_ABS && e->code < 0x40 && d->has_abs[e->code]) {
        const struct input_absinfo *a = &d->abs[e->code];
        switch (e->code) {
        case ABS_X: p->lx = norm_axis(a, e->value, false); break;
        case ABS_Y: p->ly = norm_axis(a, e->value, true); break;
        case ABS_RX: p->rx = norm_axis(a, e->value, false); break;
        case ABS_RY: p->ry = norm_axis(a, e->value, true); break;
        case ABS_Z: p->lt = norm_trigger(a, e->value); break;
        case ABS_RZ: p->rt = norm_trigger(a, e->value); break;
        case ABS_HAT0X:
            p->buttons &= ~(LEFT_FLAG | RIGHT_FLAG);
            if (e->value < 0) p->buttons |= LEFT_FLAG;
            if (e->value > 0) p->buttons |= RIGHT_FLAG;
            break;
        case ABS_HAT0Y:
            p->buttons &= ~(UP_FLAG | DOWN_FLAG);
            if (e->value < 0) p->buttons |= UP_FLAG;
            if (e->value > 0) p->buttons |= DOWN_FLAG;
            break;
        default: return;
        }
        p->dirty = true;
    } else if (e->type == EV_SYN && e->code == SYN_REPORT && p->dirty) {
        p->dirty = false;
        LiSendMultiControllerEvent((short)d->pad, pad_mask(), p->buttons, p->lt, p->rt, p->lx, p->ly, p->rx, p->ry);
    }
}

static void remote_key(const struct input_event *e) {
    int f = 0;
    switch (e->code) {
    case KEY_UP: f = UP_FLAG; break;
    case KEY_DOWN: f = DOWN_FLAG; break;
    case KEY_LEFT: f = LEFT_FLAG; break;
    case KEY_RIGHT: f = RIGHT_FLAG; break;
    case KEY_OK: case KEY_SELECT: case KEY_ENTER: f = A_FLAG; break;
    case KEY_BACK: case KEY_EXIT: f = B_FLAG; break;
    default: return;
    }
    if (remote_pad < 0) {
        for (int i = 0; i < MAX_PADS; i++)
            if (!pads[i].used) { remote_pad = i; break; }
        if (remote_pad < 0) return;
        memset(&pads[remote_pad], 0, sizeof pads[remote_pad]);
        pads[remote_pad].used = true;
        LiSendControllerArrivalEvent((uint8_t)remote_pad, (uint16_t)pad_mask(), LI_CTYPE_XBOX,
                                     A_FLAG | B_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG, 0);
        tlog("input: fjernbetjeningen er controller %d", remote_pad);
    }
    Pad *p = &pads[remote_pad];
    if (e->value) p->buttons |= f;
    else p->buttons &= ~f;
    LiSendMultiControllerEvent((short)remote_pad, pad_mask(), p->buttons, 0, 0, 0, 0, 0, 0);
}

static bool is_mouse_btn(short code) {
    return code == BTN_LEFT || code == BTN_MIDDLE || code == BTN_RIGHT || code == BTN_SIDE || code == BTN_EXTRA;
}

static void mouse_key(const struct input_event *e) {
    int b = 0;
    switch (e->code) {
    case BTN_LEFT: b = BUTTON_LEFT; break;
    case BTN_MIDDLE: b = BUTTON_MIDDLE; break;
    case BTN_RIGHT: b = BUTTON_RIGHT; break;
    case BTN_SIDE: b = BUTTON_X1; break;
    case BTN_EXTRA: b = BUTTON_X2; break;
    default: return;
    }
    LiSendMouseButtonEvent(e->value ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, b);
}

static void mouse_rel(Dev *d, const struct input_event *e) {
    switch (e->code) {
    case REL_X: d->mdx = (short)(d->mdx + e->value); break;
    case REL_Y: d->mdy = (short)(d->mdy + e->value); break;
    case REL_WHEEL: d->wheel = (signed char)(d->wheel + e->value); break;
    case REL_HWHEEL: d->hwheel = (signed char)(d->hwheel + e->value); break;
    }
}

/* one packet per SYN_REPORT, like a real mouse */
static void mouse_flush(Dev *d) {
    if (d->mdx || d->mdy) {
        LiSendMouseMoveEvent(d->mdx, d->mdy);
        d->mdx = d->mdy = 0;
    }
    if (d->wheel) { LiSendScrollEvent(d->wheel); d->wheel = 0; }
    if (d->hwheel) { LiSendHScrollEvent(d->hwheel); d->hwheel = 0; }
}

static void key_event(Dev *d, const struct input_event *e) {
    bool down = e->value != 0;

    if (e->value == 1 && e->code == KEY_GREEN) stats_toggle = true; /* Green = info: stats overlay */
    if (e->value == 1 && e->code == KEY_RED) { /* Red = menu (open/close) */
        menu_sel = 0;
        menu_open = !menu_open;
        menu_dirty = true;
        tlog("input: Red - menu %s", menu_open ? "open" : "closed");
        return;
    }
    if (menu_open && e->value != 0) { /* the menu takes arrows/OK/Back; releases are still passed on */
        if (e->value == 1) switch (e->code) {
        case KEY_UP: menu_sel = (menu_sel + MENU_N - 1) % MENU_N; menu_dirty = true; break;
        case KEY_DOWN: menu_sel = (menu_sel + 1) % MENU_N; menu_dirty = true; break;
        case KEY_OK: case KEY_SELECT: case KEY_ENTER: menu_pick = menu_sel; menu_open = false; menu_dirty = true; break;
        case KEY_BACK: case KEY_EXIT: menu_open = false; menu_dirty = true; break;
        default: break;
        }
        return;
    }

    if (d->remote) {
        if (e->value != 2) remote_key(e);
        return;
    }
    if (!d->kbd) return;

    char m = 0;
    switch (e->code) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: m = MODIFIER_SHIFT; break;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: m = MODIFIER_CTRL; break;
    case KEY_LEFTALT: case KEY_RIGHTALT: m = MODIFIER_ALT; break;
    case KEY_LEFTMETA: case KEY_RIGHTMETA: m = MODIFIER_META; break;
    }
    if (m) {
        if (down) k_mods |= m;
        else k_mods &= (char)~m;
    }

    /* quit: Ctrl + Alt + Shift + Q (the Q itself is not sent to the game) */
    if (e->value == 1 && e->code == KEY_Q && (k_mods & (MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT)) ==
                                                       (MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT)) {
        tlog("input: quit combination (keyboard)");
        quit_requested = true;
        return;
    }

    if (e->value == 2) return; /* the host repeats a held key itself */
    if (e->code >= (short)(sizeof vk_codes / sizeof vk_codes[0])) return;
    short vk = vk_codes[e->code];
    if (!vk) return;
    LiSendKeyboardEvent(vk, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, k_mods);
}

static void *input_thread(void *arg) {
    while (running) {
        struct pollfd pf[MAX_DEV + 1];
        int n = 0;
        for (int i = 0; i < n_devs; i++) pf[n++] = (struct pollfd){devs[i].fd, POLLIN, 0};
        if (ino >= 0) pf[n++] = (struct pollfd){ino, POLLIN, 0};
        if (poll(pf, (nfds_t)n, 100) <= 0) continue;
        for (int i = n_devs - 1; i >= 0; i--) {
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { close_dev(i); continue; }
            if (!(pf[i].revents & POLLIN)) continue;
            struct input_event ev[64];
            ssize_t r = read(devs[i].fd, ev, sizeof ev);
            if (r < 0) { if (errno == ENODEV) close_dev(i); continue; }
            for (size_t k = 0; k < (size_t)r / sizeof ev[0]; k++) {
                Dev *d = &devs[i];
                struct input_event *e = &ev[k];
                if (d->pad >= 0) { pad_event(d, e); continue; }
                if (d->mouse) {
                    if (e->type == EV_REL) { mouse_rel(d, e); continue; }
                    if (e->type == EV_KEY && is_mouse_btn(e->code)) { mouse_key(e); continue; }
                    if (e->type == EV_SYN && e->code == SYN_REPORT) { mouse_flush(d); continue; }
                }
                if (e->type == EV_KEY) key_event(d, e);
            }
        }
        if (ino >= 0 && (pf[n - 1].revents & POLLIN)) {
            char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t len = read(ino, buf, sizeof buf);
            for (char *p = buf; len > 0 && p < buf + len;) {
                struct inotify_event *ie = (struct inotify_event *)p;
                if (ie->len) open_dev(ie->name);
                p += sizeof *ie + ie->len;
            }
        }
    }
    return NULL;
}

bool input_start(void) {
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino >= 0) inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_ATTRIB);
    DIR *dir = opendir("/dev/input");
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) open_dev(e->d_name);
        closedir(dir);
    }
    running = true;
    return pthread_create(&th, NULL, input_thread, NULL) == 0;
}

void input_stop(void) {
    if (!running) return;
    running = false;
    pthread_join(th, NULL);
    while (n_devs) close_dev(n_devs - 1);
    remote_pad = -1;
    if (ino >= 0) close(ino);
    ino = -1;
}
