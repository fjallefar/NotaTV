/* NotaTV shell - input: evdev (keyboard, HDMI-CEC, IR, gamepad) + SDL (development on a PC) */
#include "tv.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define MAX_DEV 32
#define QLEN 64

static int dev_fd[MAX_DEV];
static char dev_name[MAX_DEV][32];
static int n_dev;
static int ino_fd = -1;
static bool evdev_on;
static bool shift_down, ctrl_down;
static InputEvent queue[QLEN];
static int q_head, q_tail;
static double last_activity;

double input_last_activity(void) { return last_activity; }
bool input_evdev_active(void) { return evdev_on; }

void input_push(Action a, uint32_t ch, bool repeat) {
    int nt = (q_tail + 1) % QLEN;
    if (nt == q_head) return; /* queue full: drop */
    queue[q_tail] = (InputEvent){a, ch, repeat};
    q_tail = nt;
    last_activity = app_time();
}

bool input_next(InputEvent *ev) {
    if (q_head == q_tail) return false;
    *ev = queue[q_head];
    q_head = (q_head + 1) % QLEN;
    return true;
}

static void open_dev(const char *name) {
    if (strncmp(name, "event", 5) != 0) return;
    for (int i = 0; i < n_dev; i++)
        if (strcmp(dev_name[i], name) == 0) return;
    if (n_dev >= MAX_DEV) return;
    char path[64];
    snprintf(path, sizeof path, "/dev/input/%s", name);
    int fd = -1;
    for (int tries = 0; tries < 20 && fd < 0; tries++) {
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 && errno == ENOENT) break;
        if (fd < 0) usleep(5000);
    }
    if (fd < 0) return;
    unsigned long evbits = 0;
    ioctl(fd, EVIOCGBIT(0, sizeof evbits), &evbits);
    if (!(evbits & (1UL << EV_KEY))) { close(fd); return; }
    char devname[128] = "?";
    ioctl(fd, EVIOCGNAME(sizeof devname), devname);
    SDL_Log("input: %s (%s)", path, devname);
    dev_fd[n_dev] = fd;
    snprintf(dev_name[n_dev], sizeof dev_name[n_dev], "%s", name);
    n_dev++;
}

static void close_dev(int i) {
    close(dev_fd[i]);
    for (int k = i; k < n_dev - 1; k++) {
        dev_fd[k] = dev_fd[k + 1];
        memcpy(dev_name[k], dev_name[k + 1], sizeof dev_name[k]);
    }
    n_dev--;
}

void input_init(bool use_evdev) {
    evdev_on = use_evdev;
    last_activity = 0;
    if (!use_evdev) return;
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino_fd >= 0) inotify_add_watch(ino_fd, "/dev/input", IN_CREATE | IN_ATTRIB);
    DIR *d = opendir("/dev/input");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) open_dev(e->d_name);
        closedir(d);
    }
}

void input_shutdown(void) {
    while (n_dev) close_dev(0);
    if (ino_fd >= 0) close(ino_fd);
    ino_fd = -1;
}

int input_fds(int *fds, int max) {
    int n = 0;
    for (int i = 0; i < n_dev && n < max; i++) fds[n++] = dev_fd[i];
    if (ino_fd >= 0 && n < max) fds[n++] = ino_fd;
    return n;
}

/* characters from the keyboard (Danish/US-like layout - enough for URLs and passwords) */
/* a physical keyboard: the Danish layout when NotaTV is in Danish (æ ø å), otherwise US - one whole layout either way */
static uint32_t key_char(int code, bool shift) {
    static const struct { int code; uint32_t lo, hi; } letters[] = {
        {KEY_Q, 'q', 'Q'}, {KEY_W, 'w', 'W'}, {KEY_E, 'e', 'E'}, {KEY_R, 'r', 'R'}, {KEY_T, 't', 'T'},
        {KEY_Y, 'y', 'Y'}, {KEY_U, 'u', 'U'}, {KEY_I, 'i', 'I'}, {KEY_O, 'o', 'O'}, {KEY_P, 'p', 'P'},
        {KEY_A, 'a', 'A'}, {KEY_S, 's', 'S'}, {KEY_D, 'd', 'D'}, {KEY_F, 'f', 'F'}, {KEY_G, 'g', 'G'},
        {KEY_H, 'h', 'H'}, {KEY_J, 'j', 'J'}, {KEY_K, 'k', 'K'}, {KEY_L, 'l', 'L'}, {KEY_Z, 'z', 'Z'},
        {KEY_X, 'x', 'X'}, {KEY_C, 'c', 'C'}, {KEY_V, 'v', 'V'}, {KEY_B, 'b', 'B'}, {KEY_N, 'n', 'N'},
        {KEY_M, 'm', 'M'}, {KEY_SPACE, ' ', ' '},
    };
    static const struct { int code; uint32_t lo, hi; } us[] = {
        {KEY_MINUS, '-', '_'}, {KEY_EQUAL, '=', '+'}, {KEY_DOT, '.', '>'}, {KEY_COMMA, ',', '<'}, {KEY_SLASH, '/', '?'},
        {KEY_SEMICOLON, ';', ':'}, {KEY_APOSTROPHE, '\'', '"'}, {KEY_BACKSLASH, '\\', '|'}, {KEY_GRAVE, '`', '~'},
        {KEY_LEFTBRACE, '[', '{'}, {KEY_RIGHTBRACE, ']', '}'}, {KEY_102ND, '\\', '|'},
    }, da[] = {
        {KEY_MINUS, '+', '?'}, {KEY_EQUAL, 0xB4, '`'}, {KEY_DOT, '.', ':'}, {KEY_COMMA, ',', ';'}, {KEY_SLASH, '-', '_'},
        {KEY_SEMICOLON, 0xE6, 0xC6}, {KEY_APOSTROPHE, 0xF8, 0xD8}, {KEY_BACKSLASH, '\'', '*'}, {KEY_GRAVE, 0xBD, 0xA7},
        {KEY_LEFTBRACE, 0xE5, 0xC5}, {KEY_RIGHTBRACE, 0xA8, '^'}, {KEY_102ND, '<', '>'},
    };
    bool dk = !strcmp(i18n_lang(), "da");
    if (code >= KEY_1 && code <= KEY_0) {
        static const char *row1 = "1234567890", *row1_us = "!@#$%^&*()";
        static const uint32_t row1_da[] = {'!', '"', '#', 0xA4, '%', '&', '/', '(', ')', '='};
        int k = code - KEY_1;
        return !shift ? (uint32_t)row1[k] : dk ? row1_da[k] : (uint32_t)row1_us[k];
    }
    for (size_t i = 0; i < sizeof letters / sizeof letters[0]; i++)
        if (letters[i].code == code) return shift ? letters[i].hi : letters[i].lo;
    size_t n = dk ? sizeof da / sizeof da[0] : sizeof us / sizeof us[0];
    for (size_t i = 0; i < n; i++) {
        int c = dk ? da[i].code : us[i].code;
        if (c == code) return dk ? (shift ? da[i].hi : da[i].lo) : (shift ? us[i].hi : us[i].lo);
    }
    return 0;
}

/* software autorepeat: LG's HDMI-CEC only sends press + release (no repeats), so "hold OK" and
   held arrow keys are repeated here - only if the device does not send repeats itself (value 2) */
static int held_code;
static double held_t0, held_last;
static bool held_hw_rep;
/* ... and when the LG remote is held down, it sends new presses instead: the first after ~0.5 s, then every ~0.25 s.
   The same OK/Back again within HOLD_GAP therefore counts as "held" (repeat). Held Back (3rd press) = Home. */
static int last_code, back_n;
static double last_press_t;

static void handle_key(int code, int value) {
    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) { shift_down = value != 0; return; }
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) { ctrl_down = value != 0; return; }
    if (value == 0) return; /* only press + autorepeat */
    bool rep = value == 2;
    if (value == 1) {
        double now = app_time();
        bool again = code == last_code && now - last_press_t < HOLD_GAP;
        last_code = code;
        last_press_t = now;
        if (again && (code == KEY_OK || code == KEY_SELECT)) rep = true;
        if (code == KEY_BACK || code == KEY_EXIT) {
            back_n = again ? back_n + 1 : 1;
            if (back_n >= 3) { back_n = 0; input_push(A_HOME, 0, false); return; }
            rep = again;
        }
    }
    Action a = A_NONE;
    uint32_t ch = 0;
    switch (code) {
    case KEY_UP: a = A_UP; break;
    case KEY_DOWN: a = A_DOWN; break;
    case KEY_LEFT: a = A_LEFT; break;
    case KEY_RIGHT: a = A_RIGHT; break;
    case KEY_ENTER: case KEY_KPENTER: a = A_OK; ch = '\n'; break; /* fysisk Enter */
    case KEY_OK: case KEY_SELECT: case BTN_SOUTH: a = A_OK; break;
    case KEY_ESC: case KEY_BACK: case KEY_EXIT: case BTN_EAST: case KEY_CANCEL: a = A_BACK; break;
    case KEY_HOME: case KEY_HOMEPAGE: case KEY_ROOT_MENU: case BTN_MODE: case KEY_TV: a = A_HOME; break;
    case KEY_MENU: case KEY_CONTEXT_MENU: case KEY_SETUP: case KEY_OPTION: case BTN_START:
    case KEY_COMPOSE: a = A_MENU; break;
    case KEY_PLAYPAUSE: case KEY_PLAYCD: case KEY_PAUSECD: a = A_PLAYPAUSE; break;
    case KEY_PLAY: a = A_PLAY; break;
    case KEY_PAUSE: a = A_PAUSE; break;
    case KEY_STOP: case KEY_STOPCD: a = A_STOP; break;
    case KEY_FASTFORWARD: case KEY_FORWARD: a = A_FF; break;
    case KEY_REWIND: a = A_REW; break;
    case KEY_NEXTSONG: case KEY_NEXT: a = A_NEXT; break;
    case KEY_PREVIOUSSONG: case KEY_PREVIOUS: a = A_PREV; break;
    case KEY_CHANNELUP: case KEY_PAGEUP: a = A_CHUP; break;
    case KEY_CHANNELDOWN: case KEY_PAGEDOWN: a = A_CHDOWN; break;
    case KEY_INFO: case KEY_EPG: case KEY_PROGRAM: case KEY_F1: a = A_INFO; break;
    /* the color keys have fixed meanings across the whole system (LG sends neither Menu, Info nor Play over CEC) */
    case KEY_RED: case KEY_F2: a = A_MENU; break;     /* red = menu / settings */
    case KEY_GREEN: case KEY_F3: a = A_INFO; break;   /* green = info */
    case KEY_YELLOW: case KEY_F4: a = A_YELLOW; break; /* yellow = options */
    case KEY_BLUE: case KEY_F5: a = A_SEARCH; break;  /* blue = search */
    case KEY_BACKSPACE: a = A_BACKSPACE; break;
    case KEY_POWER: case KEY_SLEEP: case KEY_SUSPEND: case KEY_POWER2: a = A_POWER; break;
    case KEY_VOLUMEUP: a = A_VOLUP; break;
    case KEY_VOLUMEDOWN: a = A_VOLDOWN; break;
    case KEY_MUTE: a = A_MUTE; break;
    case KEY_SUBTITLE: case KEY_TEXT: a = A_SUBTITLE; break;
    case KEY_AUDIO: case KEY_LANGUAGE: a = A_AUDIO; break;
    case KEY_NUMERIC_0 ... KEY_NUMERIC_9: a = A_DIGIT; ch = '0' + (code - KEY_NUMERIC_0); break;
    case KEY_KP0: a = A_DIGIT; ch = '0'; break;
    case KEY_KP1 ... KEY_KP3: a = A_DIGIT; ch = '1' + (code - KEY_KP1); break;
    case KEY_KP4 ... KEY_KP6: a = A_DIGIT; ch = '4' + (code - KEY_KP4); break;
    case KEY_KP7 ... KEY_KP9: a = A_DIGIT; ch = '7' + (code - KEY_KP7); break;
    default:
        ch = key_char(code, shift_down);
        if (ch) {
            /* ctrl+q = quit (development) */
            if (ctrl_down && ch == 'q') { app_quit(); return; }
            a = A_CHAR;
        }
        break;
    }
    if (a != A_NONE) input_push(a, ch, rep);
}

void input_poll(void) {
    if (!evdev_on) return;
    if (ino_fd >= 0) {
        char buf[4096] __attribute__((aligned(8)));
        ssize_t n;
        while ((n = read(ino_fd, buf, sizeof buf)) > 0) {
            for (char *p = buf; p < buf + n;) {
                struct inotify_event *ev = (struct inotify_event *)p;
                if (ev->len) open_dev(ev->name);
                p += sizeof *ev + ev->len;
            }
        }
    }
    for (int i = 0; i < n_dev; i++) {
        struct input_event ev[32];
        ssize_t n;
        while ((n = read(dev_fd[i], ev, sizeof ev)) > 0) {
            for (int k = 0; k < (int)(n / sizeof ev[0]); k++) {
                if (ev[k].type == EV_KEY) {
                    int c = ev[k].code, v = ev[k].value;
                    if (v == 1) { held_code = c; held_t0 = held_last = app_time(); held_hw_rep = false; }
                    else if (v == 2 && c == held_code) held_hw_rep = true;
                    else if (v == 0 && c == held_code) held_code = 0;
                    handle_key(c, v);
                }
                else if (ev[k].type == EV_ABS && (ev[k].code == ABS_HAT0X || ev[k].code == ABS_HAT0Y)) {
                    /* gamepad D-pad */
                    if (ev[k].code == ABS_HAT0X && ev[k].value) input_push(ev[k].value < 0 ? A_LEFT : A_RIGHT, 0, false);
                    if (ev[k].code == ABS_HAT0Y && ev[k].value) input_push(ev[k].value < 0 ? A_UP : A_DOWN, 0, false);
                }
            }
        }
        if (n < 0 && errno == ENODEV) { /* frakoblet */
            SDL_Log("input: %s fjernet", dev_name[i]);
            close_dev(i);
            i--;
        }
    }
    double now = app_time();
    if (held_code && !held_hw_rep) {
        if (now - held_t0 > 4) held_code = 0; /* lost release message: stop repeating after 4 s */
        else if (now - held_t0 > 0.35 && now - held_last > 0.11) { held_last = now; handle_key(held_code, 2); }
    }
}

/* SDL keyboard (development PC only) */
void input_handle_sdl(const SDL_Event *e) {
    if (evdev_on) return;
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        const char *s = e->text.text;
        /* simple UTF-8 -> codepoint */
        const unsigned char *u = (const unsigned char *)s;
        uint32_t cp = u[0];
        if ((cp & 0xE0) == 0xC0) cp = ((cp & 0x1F) << 6) | (u[1] & 0x3F);
        else if ((cp & 0xF0) == 0xE0) cp = ((cp & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        if (cp >= 32) input_push(A_CHAR, cp, false);
        return;
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return;
    bool rep = e->key.repeat;
    switch (e->key.key) {
    case SDLK_UP: input_push(A_UP, 0, rep); break;
    case SDLK_DOWN: input_push(A_DOWN, 0, rep); break;
    case SDLK_LEFT: input_push(A_LEFT, 0, rep); break;
    case SDLK_RIGHT: input_push(A_RIGHT, 0, rep); break;
    case SDLK_RETURN: case SDLK_KP_ENTER: input_push(A_OK, '\n', rep); break;
    case SDLK_ESCAPE: input_push(A_BACK, 0, rep); break;
    case SDLK_BACKSPACE: input_push(A_BACKSPACE, 0, rep); break;
    case SDLK_HOME: input_push(A_HOME, 0, rep); break;
    case SDLK_F1: input_push(A_INFO, 0, rep); break;
    /* F2-F5 = the color keys, the same as on the box (A_RED/A_GREEN/A_BLUE are not used anywhere) */
    case SDLK_F2: input_push(A_MENU, 0, rep); break;
    case SDLK_F3: input_push(A_INFO, 0, rep); break;
    case SDLK_F4: input_push(A_YELLOW, 0, rep); break;
    case SDLK_F5: input_push(A_SEARCH, 0, rep); break;
    case SDLK_F10: input_push(A_MENU, 0, rep); break;
    case SDLK_PAGEUP: input_push(A_CHUP, 0, rep); break;
    case SDLK_PAGEDOWN: input_push(A_CHDOWN, 0, rep); break;
    case SDLK_Q:
        if (e->key.mod & SDL_KMOD_CTRL) app_quit();
        break;
    default: break;
    }
}
