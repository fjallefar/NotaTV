/* tvmoon - input: evdev -> the host.
 *
 * Controllers (up to 4) are sent as Xbox-like controllers. Quit the stream:
 *   controller: Select + Start + LB + RB (like Moonlight)
 *   keyboard:   Ctrl + Alt + Shift + Q
 *   remote (CEC): Red = menu (Resume / Stats / Quit). Green = stats overlay
 *   (holding Back goes to LG's own menu)
 * The remote's arrows/OK/Back are sent as a controller (d-pad/A/B), so you can navigate without a controller.
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
static bool k_ctrl, k_alt, k_shift;
static int remote_pad = -1; /* controller number for the remote (created on the first press) */

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
    tlog("input: %s (%s)%s", path, devname, d->pad >= 0 ? " = controller" : "");
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

static void key_event(Dev *d, const struct input_event *e) {
    bool down = e->value != 0;
    switch (e->code) {
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: k_ctrl = down; return;
    case KEY_LEFTALT: case KEY_RIGHTALT: k_alt = down; return;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: k_shift = down; return;
    }
    if (e->value == 1 && e->code == KEY_Q && k_ctrl && k_alt && k_shift) {
        tlog("input: afslut-kombination (tastatur)");
        quit_requested = true;
    }
    if (e->value == 1 && e->code == KEY_GREEN) stats_toggle = true; /* Green = info: stats overlay */
    if (e->value == 1 && e->code == KEY_RED) { /* Red = menu (open/close) */
        menu_sel = 0;
        menu_open = !menu_open;
        menu_dirty = true;
        tlog("input: Rød - menu %s", menu_open ? "åben" : "lukket");
        return;
    }
    if (menu_open && d->remote && e->value != 0) { /* the menu takes arrows/OK/Back; releases are still passed on */
        if (e->value == 1) switch (e->code) {
        case KEY_UP: menu_sel = (menu_sel + MENU_N - 1) % MENU_N; menu_dirty = true; break;
        case KEY_DOWN: menu_sel = (menu_sel + 1) % MENU_N; menu_dirty = true; break;
        case KEY_OK: case KEY_SELECT: case KEY_ENTER: menu_pick = menu_sel; menu_open = false; menu_dirty = true; break;
        case KEY_BACK: case KEY_EXIT: menu_open = false; menu_dirty = true; break;
        default: break;
        }
        return;
    }
    if (d->remote && e->value != 2) remote_key(e);
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
                if (devs[i].pad >= 0) pad_event(&devs[i], &ev[k]);
                else if (ev[k].type == EV_KEY) key_event(&devs[i], &ev[k]);
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
