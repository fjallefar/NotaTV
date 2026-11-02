/* NotaTV shell - dialogs: on-screen keyboard, confirm, choice list, power menu */
#include "tv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void overlay_dim(float a) { fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(170 * a))); }

/* ================= On-screen keyboard ================= */
typedef struct {
    char title[128];
    char text[1024];
    bool password, shift, show_pw;
    bool typed; /* typed with a physical keyboard: Enter = Done */
    int row, col;
    KbDone cb;
    void *ud;
} Kb;

/* the Danish keyboard has æøå; the English one has ' and ( ) in those places */
static const char *kb_rows_lo_da[] = {"1234567890", "qwertyuiopå", "asdfghjklæø", "zxcvbnm-_.@", ":/?&=#%+~!,"};
static const char *kb_rows_hi_da[] = {"1234567890", "QWERTYUIOPÅ", "ASDFGHJKLÆØ", "ZXCVBNM-_.@", ":/?&=#%+~!,"};
static const char *kb_rows_lo_en[] = {"1234567890", "qwertyuiop'", "asdfghjkl()", "zxcvbnm-_.@", ":/?&=#%+~!,"};
static const char *kb_rows_hi_en[] = {"1234567890", "QWERTYUIOP\"", "ASDFGHJKL[]", "ZXCVBNM-_.@", ":/?&=#%+~!,"};
#define kb_da (!strcmp(i18n_lang(), "da"))
#define kb_rows_lo (kb_da ? kb_rows_lo_da : kb_rows_lo_en)
#define kb_rows_hi (kb_da ? kb_rows_hi_da : kb_rows_hi_en)
#define KB_ROWS 5
#define KB_SPECIAL 5 /* last row: shift, space, delete, clear, ok */
static Screen *kb_screen;

static int utf8_len(const char *s) {
    int n = 0;
    while (*s) { if ((*s & 0xC0) != 0x80) n++; s++; }
    return n;
}

static const char *utf8_at(const char *s, int idx, int *bytes) {
    int i = 0;
    while (*s) {
        int l = 1;
        unsigned char c = (unsigned char)*s;
        if (c >= 0xF0) l = 4; else if (c >= 0xE0) l = 3; else if (c >= 0xC0) l = 2;
        if (i == idx) { *bytes = l; return s; }
        s += l;
        i++;
    }
    *bytes = 0;
    return NULL;
}

static void kb_append(Kb *k, const char *s) {
    size_t a = strlen(k->text), b = strlen(s);
    if (a + b < sizeof k->text - 1) memcpy(k->text + a, s, b + 1);
}

static void kb_backspace(Kb *k) {
    size_t n = strlen(k->text);
    while (n > 0) {
        n--;
        if (((unsigned char)k->text[n] & 0xC0) != 0x80) break;
    }
    k->text[n] = 0;
}

static void kb_finish(Screen *s, bool ok) {
    Kb *k = s->st;
    KbDone cb = k->cb;
    void *ud = k->ud;
    char *txt = ok ? str_dup(k->text) : NULL;
    kb_screen = NULL;
    app_pop();
    if (cb) cb(ud, txt);
    free(txt);
}

static void kb_draw(Screen *s) {
    Kb *k = s->st;
    overlay_dim(s->anim);
    float pw = 1240, ph = 770, px = UI_W / 2 - pw / 2, py = UI_H / 2 - ph / 2 + 20;
    draw_shadow(px, py, pw, ph, 28, 40, s->anim);
    fill_rrect(px, py, pw, ph, 28, RGB(24, 28, 37));
    text_draw(F_BOLD, 34, px + 50, py + 38, C_TEXT, AL_LEFT, k->title);
    /* tekstfelt */
    fill_rrect(px + 50, py + 100, pw - 100, 78, 14, RGB(12, 14, 19));
    char shown[1100];
    if (k->password && !k->show_pw) {
        int n = utf8_len(k->text);
        int i;
        for (i = 0; i < n && i < 200; i++) memcpy(shown + i * 3, "•", 3);
        shown[i * 3] = 0;
    } else snprintf(shown, sizeof shown, "%s", k->text);
    float tw = text_width(F_REG, 32, shown);
    float maxw = pw - 140;
    clip_push(px + 70, py + 100, maxw + 10, 78);
    float tx = px + 70 + (tw > maxw ? maxw - tw : 0);
    text_draw(F_REG, 32, tx, py + 120, C_TEXT, AL_LEFT, shown);
    if (((int)(app_time() * 2)) % 2 == 0) fill_rect(tx + tw + 3, py + 118, 3, 42, C_ACCENT);
    clip_pop();
    if (!k->text[0]) text_draw(F_REG, 26, px + 70, py + 124, C_TEXT3, AL_LEFT, T("Use the remote, a keyboard or your phone"));

    const char **rows = k->shift ? kb_rows_hi : kb_rows_lo;
    float key = 84, gap = 10, ky = py + 210;
    for (int r = 0; r < KB_ROWS; r++) {
        int n = utf8_len(rows[r]);
        float rw = n * key + (n - 1) * gap;
        float kx = UI_W / 2 - rw / 2;
        for (int c = 0; c < n; c++) {
            int bl;
            const char *ch = utf8_at(rows[r], c, &bl);
            char buf[8] = {0};
            memcpy(buf, ch, bl);
            bool f = k->row == r && k->col == c;
            float x = kx + c * (key + gap), y = ky + r * (key * 0.78f + gap);
            fill_rrect(x, y, key, key * 0.78f, 12, f ? C_FOCUS : RGB(44, 50, 64));
            text_draw(F_SEMI, 30, x + key / 2, y + 14, f ? RGB(10, 10, 10) : C_TEXT, AL_CENTER, buf);
        }
    }
    static const char *sp[KB_SPECIAL] = {"ABC", N_("Space"), N_("Delete"), N_("Clear"), N_("Done")};
    float sw[KB_SPECIAL] = {170, 380, 170, 140, 190};
    float total = 0;
    for (int i = 0; i < KB_SPECIAL; i++) total += sw[i] + (i ? gap : 0);
    float sx = UI_W / 2 - total / 2, sy = ky + KB_ROWS * (key * 0.78f + gap) + 6;
    for (int i = 0; i < KB_SPECIAL; i++) {
        bool f = k->row == KB_ROWS && k->col == i;
        Color bg = i == 4 ? C_ACCENT : RGB(44, 50, 64);
        if (i == 0 && k->shift) bg = RGB(70, 80, 100);
        fill_rrect(sx, sy, sw[i], key * 0.78f, 12, f ? C_FOCUS : bg);
        const char *lbl = T(sp[i]);
        if (i == 0) lbl = k->shift ? "abc" : "ABC";
        text_draw(F_SEMI, 26, sx + sw[i] / 2, sy + 17, f ? RGB(10, 10, 10) : C_TEXT, AL_CENTER, lbl);
        sx += sw[i] + gap;
    }
    if (k->password) draw_keybar(px + 50, py + ph - 56, NULL, NULL, k->show_pw ? T("Hide password") : T("Show password"), NULL, NULL);
    char hint[160];
    const char *ip = app_ip();
    if (ip[0]) {
        snprintf(hint, sizeof hint, T("Type from your phone: http://%s"), ip);
        text_draw(F_REG, 22, px + pw - 50, py + ph - 52, C_TEXT3, AL_RIGHT, hint);
    }
}

static int kb_row_len(Kb *k, int r) {
    if (r == KB_ROWS) return KB_SPECIAL;
    return utf8_len((k->shift ? kb_rows_hi : kb_rows_lo)[r]);
}

static bool kb_input(Screen *s, const InputEvent *ev) {
    Kb *k = s->st;
    switch (ev->a) {
    case A_UP:
        if (k->row > 0) {
            int old = kb_row_len(k, k->row);
            k->row--;
            k->col = k->col * kb_row_len(k, k->row) / (old ? old : 1);
        }
        break;
    case A_DOWN:
        if (k->row < KB_ROWS) {
            int old = kb_row_len(k, k->row);
            k->row++;
            k->col = k->col * kb_row_len(k, k->row) / (old ? old : 1);
        } else return false; /* bottom row: the key bar (Show password) */
        break;
    case A_LEFT: k->col = (k->col + kb_row_len(k, k->row) - 1) % kb_row_len(k, k->row); break;
    case A_RIGHT: k->col = (k->col + 1) % kb_row_len(k, k->row); break;
    case A_OK:
        if (ev->repeat) return true; /* OK held down: no repeated letters */
        if (ev->ch == '\n' && k->typed) { kb_finish(s, true); return true; }
        if (k->row < KB_ROWS) {
            int bl;
            const char *ch = utf8_at((k->shift ? kb_rows_hi : kb_rows_lo)[k->row], k->col, &bl);
            char buf[8] = {0};
            memcpy(buf, ch, bl);
            kb_append(k, buf);
        } else {
            switch (k->col) {
            case 0: k->shift = !k->shift; break;
            case 1: kb_append(k, " "); break;
            case 2: kb_backspace(k); break;
            case 3: k->text[0] = 0; break;
            case 4: kb_finish(s, true); return true;
            }
        }
        break;
    case A_BACKSPACE: kb_backspace(k); break;
    case A_YELLOW: k->show_pw = !k->show_pw; break;
    case A_CHAR: case A_DIGIT: {
        if (ev->a == A_CHAR) k->typed = true;
        char buf[8] = {0};
        uint32_t c = ev->ch;
        if (c < 0x80) buf[0] = (char)c;
        else if (c < 0x800) { buf[0] = 0xC0 | (c >> 6); buf[1] = 0x80 | (c & 0x3F); }
        else { buf[0] = 0xE0 | (c >> 12); buf[1] = 0x80 | ((c >> 6) & 0x3F); buf[2] = 0x80 | (c & 0x3F); }
        kb_append(k, buf);
        break;
    }
    case A_PLAYPAUSE: kb_finish(s, true); return true;
    case A_BACK: kb_finish(s, false); return true;
    case A_HOME: kb_finish(s, false); return false;
    default: return false;
    }
    return true;
}

void keyboard_open(const char *title, const char *initial, bool password, KbDone cb, void *ud) {
    Screen *s = calloc(1, sizeof *s);
    Kb *k = calloc(1, sizeof *k);
    snprintf(k->title, sizeof k->title, "%s", title);
    if (initial) snprintf(k->text, sizeof k->text, "%s", initial);
    k->password = password;
    k->cb = cb;
    k->ud = ud;
    k->row = 1;
    s->name = "keyboard";
    s->st = k;
    s->draw = kb_draw;
    s->input = kb_input;
    s->overlay = true;
    kb_screen = s;
    if (!input_evdev_active()) SDL_StartTextInput(SDL_GetKeyboardFocus());
    app_push(s);
}

void keyboard_inject(const char *text) {
    if (!kb_screen) { app_toast(T("Open a text field on the TV first")); return; }
    Kb *k = kb_screen->st;
    snprintf(k->text, sizeof k->text, "%s", text);
    app_toast(T("Text received from phone"));
}

/* ================= Confirm dialog ================= */
typedef struct {
    char title[128], msg[512], yes[64], no[64];
    int sel;
    ConfirmDone cb;
    void *ud;
} Confirm;

static void confirm_draw(Screen *s) {
    Confirm *c = s->st;
    overlay_dim(s->anim);
    float pw = 900, ph = 380, px = UI_W / 2 - pw / 2, py = UI_H / 2 - ph / 2;
    draw_shadow(px, py, pw, ph, 28, 40, s->anim);
    fill_rrect(px, py, pw, ph, 28, RGB(24, 28, 37));
    text_draw(F_BOLD, 36, UI_W / 2, py + 44, C_TEXT, AL_CENTER, c->title);
    text_draw_wrap(F_REG, 26, px + 60, py + 110, pw - 120, 4, 36, C_TEXT2, c->msg);
    const char *lbl[2] = {c->yes, c->no};
    for (int i = 0; i < 2; i++) {
        float bw = 300, bx = UI_W / 2 + (i == 0 ? -bw - 15 : 15), by = py + ph - 110;
        bool f = c->sel == i;
        fill_rrect(bx, by, bw, 68, 34, f ? C_FOCUS : RGB(44, 50, 64));
        text_draw(F_SEMI, 28, bx + bw / 2, by + 18, f ? RGB(10, 10, 10) : C_TEXT, AL_CENTER, lbl[i]);
    }
}

static bool confirm_input(Screen *s, const InputEvent *ev) {
    Confirm *c = s->st;
    if (ev->a == A_LEFT || ev->a == A_RIGHT) { c->sel ^= 1; return true; }
    if (ev->a == A_OK && ev->repeat) return true;
    if (ev->a == A_OK || ev->a == A_BACK) {
        bool yes = ev->a == A_OK && c->sel == 0;
        ConfirmDone cb = c->cb;
        void *ud = c->ud;
        app_pop();
        if (cb) cb(ud, yes);
        return true;
    }
    return ev->a != A_HOME;
}

void confirm_open(const char *title, const char *msg, const char *yes, const char *no, ConfirmDone cb, void *ud) {
    Screen *s = calloc(1, sizeof *s);
    Confirm *c = calloc(1, sizeof *c);
    snprintf(c->title, sizeof c->title, "%s", title);
    snprintf(c->msg, sizeof c->msg, "%s", msg ? msg : "");
    snprintf(c->yes, sizeof c->yes, "%s", yes ? yes : T("Yes"));
    snprintf(c->no, sizeof c->no, "%s", no ? no : T("Cancel"));
    c->sel = 1; /* safe choice by default */
    c->cb = cb;
    c->ud = ud;
    s->name = "confirm";
    s->st = c;
    s->draw = confirm_draw;
    s->input = confirm_input;
    s->overlay = true;
    app_push(s);
}

/* ================= Next-up: "next episode" countdown (auto-advance) ================= */
typedef struct {
    char *poster, *title, *subtitle;
    double deadline;
    NextUpDone cb;
    void *ud;
} NextUp;

static void nextup_finish(Screen *s, bool play) {
    NextUp *n = s->st;
    NextUpDone cb = n->cb;
    void *ud = n->ud;
    app_pop(); /* frees n */
    if (cb) cb(ud, play);
}

static void nextup_update(Screen *s, float dt) {
    (void)dt;
    NextUp *n = s->st;
    if (app_time() >= n->deadline) nextup_finish(s, true);
}

static void nextup_draw(Screen *s) {
    NextUp *n = s->st;
    double left = n->deadline - app_time();
    int secs = left > 0 ? (int)left + (left > (int)left ? 1 : 0) : 0;
    float w = 760, h = 250, x = UI_W - w - 60, y = UI_H - h - 60;
    draw_shadow(x, y, w, h, 18, 30, s->anim);
    fill_rrect(x, y, w, h, 18, RGBA(18, 20, 28, 246));
    stroke_rrect(x, y, w, h, 18, 3, C_ACCENT);
    if (n->poster) img_draw_cover(img_get(n->poster), x + 24, y + 24, 136, 202, 10, 1);
    float tx = x + 190, ty = y + 30;
    text_draw(F_SEMI, 26, tx, ty, C_TEXT2, AL_LEFT, T("Next episode"));
    text_draw_clip(F_BOLD, 38, tx, ty + 44, w - 230, C_TEXT, AL_LEFT, n->title ? n->title : "");
    text_draw_clip(F_REG, 26, tx, ty + 96, w - 230, C_TEXT2, AL_LEFT, n->subtitle ? n->subtitle : "");
    char c[40];
    snprintf(c, sizeof c, T("Starts in %d s"), secs);
    text_draw(F_SEMI, 30, tx, y + h - 44, C_ACCENT, AL_LEFT, c);
    text_draw(F_REG, 24, tx, y + h - 84, C_TEXT3, AL_LEFT, T("OK: play now · Back: cancel"));
}

static bool nextup_input(Screen *s, const InputEvent *ev) {
    if ((ev->a == A_OK || ev->a == A_PLAY || ev->a == A_PLAYPAUSE) && !ev->repeat) { nextup_finish(s, true); return true; }
    if (ev->a == A_BACK) { nextup_finish(s, false); return true; }
    return ev->a != A_HOME; /* swallow other keys while the countdown is up */
}

static void nextup_destroy(Screen *s) {
    NextUp *n = s->st;
    free(n->poster);
    free(n->title);
    free(n->subtitle);
    free(n);
    free(s);
}

void nextup_open(const char *poster, const char *title, const char *subtitle, int seconds, NextUpDone cb, void *ud) {
    Screen *s = calloc(1, sizeof *s);
    NextUp *n = calloc(1, sizeof *n);
    n->poster = str_dup(poster);
    n->title = str_dup(title);
    n->subtitle = str_dup(subtitle);
    n->deadline = app_time() + (seconds > 0 ? seconds : 10);
    n->cb = cb;
    n->ud = ud;
    s->name = "nextup";
    s->st = n;
    s->update = nextup_update;
    s->draw = nextup_draw;
    s->input = nextup_input;
    s->destroy = nextup_destroy;
    s->overlay = true;
    app_push(s);
}

/* ================= Valgliste ================= */
typedef struct {
    char title[128];
    char **items;
    int n, sel, scroll;
    ChoiceDone cb;
    void *ud;
} Choice;

static void choice_draw(Screen *s) {
    Choice *c = s->st;
    overlay_dim(s->anim);
    int vis = c->n < 9 ? c->n : 9;
    float pw = 760, ph = 150 + vis * 72, px = UI_W / 2 - pw / 2, py = UI_H / 2 - ph / 2;
    draw_shadow(px, py, pw, ph, 28, 40, s->anim);
    fill_rrect(px, py, pw, ph, 28, RGB(24, 28, 37));
    text_draw(F_BOLD, 34, px + 50, py + 40, C_TEXT, AL_LEFT, c->title);
    if (c->sel < c->scroll) c->scroll = c->sel;
    if (c->sel >= c->scroll + vis) c->scroll = c->sel - vis + 1;
    for (int i = 0; i < vis; i++) {
        int idx = c->scroll + i;
        float y = py + 110 + i * 72;
        bool f = idx == c->sel;
        if (f) fill_rrect(px + 30, y, pw - 60, 62, 14, C_FOCUS);
        text_draw_clip(F_SEMI, 28, px + 60, y + 15, pw - 120, f ? RGB(10, 10, 10) : C_TEXT, AL_LEFT, c->items[idx]);
    }
    if (c->n > vis) {
        char pos[48];
        snprintf(pos, sizeof pos, T("%d / %d   ·   ← → pages"), c->sel + 1, c->n);
        text_draw(F_REG, 22, px + pw - 50, py + 48, C_TEXT3, AL_RIGHT, pos);
    }
}

static void choice_destroy(Screen *s) {
    Choice *c = s->st;
    for (int i = 0; i < c->n; i++) free(c->items[i]);
    free(c->items);
    free(c);
    free(s);
}

static bool choice_input(Screen *s, const InputEvent *ev) {
    Choice *c = s->st;
    if (ev->a == A_UP) { c->sel = (c->sel + c->n - 1) % c->n; return true; }
    if (ev->a == A_DOWN) { c->sel = (c->sel + 1) % c->n; return true; }
    /* long lists (e.g. countries): left/right and CH-/CH+ jump a page */
    if (ev->a == A_LEFT || ev->a == A_CHUP) { c->sel = c->sel > 9 ? c->sel - 9 : 0; return true; }
    if (ev->a == A_RIGHT || ev->a == A_CHDOWN) { c->sel = c->sel + 9 < c->n ? c->sel + 9 : c->n - 1; return true; }
    if (ev->a == A_CHAR && ev->ch > ' ' && ev->ch < 128) { /* keyboard: jump to the first item with that letter */
        int ch = ev->ch | 0x20;
        for (int k = 1; k <= c->n; k++) {
            int i = (c->sel + k) % c->n;
            if ((c->items[i][0] | 0x20) == ch) { c->sel = i; break; }
        }
        return true;
    }
    if (ev->a == A_OK && ev->repeat) return true;
    if (ev->a == A_OK || ev->a == A_BACK) {
        int r = ev->a == A_OK ? c->sel : -1;
        ChoiceDone cb = c->cb;
        void *ud = c->ud;
        app_pop();
        if (cb) cb(ud, r);
        return true;
    }
    return ev->a != A_HOME;
}

void choice_open(const char *title, const char *const *items, int n, int sel, ChoiceDone cb, void *ud) {
    if (n <= 0) return;
    Screen *s = calloc(1, sizeof *s);
    Choice *c = calloc(1, sizeof *c);
    snprintf(c->title, sizeof c->title, "%s", title);
    c->items = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) c->items[i] = str_dup(items[i]);
    c->n = n;
    c->sel = sel >= 0 && sel < n ? sel : 0;
    c->cb = cb;
    c->ud = ud;
    s->name = "choice";
    s->st = c;
    s->draw = choice_draw;
    s->input = choice_input;
    s->destroy = choice_destroy;
    s->overlay = true;
    app_push(s);
}

/* ================= Power ================= */
static void power_cb(void *ud, int idx) {
    (void)ud;
    switch (idx) {
    case 0: run_cmd("%s/power sleep &", helper_path()); break;
    case 1: run_cmd("%s/power reboot &", helper_path()); break;
    case 2: run_cmd("%s/power poweroff &", helper_path()); break;
    default: break;
    }
}

void power_menu_open(void) {
    Screen *t = app_top();
    if (t && t->name && !strcmp(t->name, "choice")) return;
    static const char *items[3];
    items[0] = T("Sleep (fast start)"); items[1] = T("Restart"); items[2] = T("Power off");
    choice_open(T("Power"), items, 3, 0, power_cb, NULL);
}

/* ================= Sidemenu ================= */
void side_init(SideMenu *m, const char *title, IconId logo, Color accent) {
    memset(m, 0, sizeof *m);
    snprintf(m->title, sizeof m->title, "%s", title);
    m->logo = logo;
    m->accent = accent;
}

void side_clear(SideMenu *m) { m->n = 0; }

int side_add(SideMenu *m, const char *label, IconId icon, bool gap_before) {
    if (m->n >= 40) return -1;
    SideItem *it = &m->items[m->n];
    snprintf(it->label, sizeof it->label, "%s", label);
    it->icon = icon;
    it->gap_before = gap_before;
    return m->n++;
}

void side_open(SideMenu *m) {
    m->open = true;
    m->sel = m->active < m->n ? m->active : 0;
}

void side_update(SideMenu *m, float dt) {
    m->t = anim_to(m->t, m->open ? 1 : 0, dt, 16);
    if (m->sel >= m->n) m->sel = m->n ? m->n - 1 : 0;
    /* scroll if there are many items */
    float item_h = 72, top = 190, maxh = UI_H - top - 50;
    float gaps = 0;
    for (int i = 0; i < m->n; i++) gaps += m->items[i].gap_before ? 28 : 0;
    float total = m->n * item_h + gaps, target = 0;
    if (total > maxh) {
        float pos = m->sel * item_h;
        target = pos - maxh / 2;
        if (target < 0) target = 0;
        if (target > total - maxh) target = total - maxh;
    }
    m->scroll = anim_to(m->scroll, target, dt, 14);
}

void side_draw(SideMenu *m) {
    float t = m->t;
    float w = SIDE_W + (SIDE_OPEN_W - SIDE_W) * t;
    /* dim the content when the menu is open */
    if (t > 0.01f) fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (uint8_t)(150 * t)));
    fill_hgrad(0, 0, w + 60 * t, UI_H, RGBA(16, 18, 25, (uint8_t)(235 + 20 * t)), RGBA(16, 18, 25, (uint8_t)(235 * t)));
    fill_rect(0, 0, w, UI_H, RGBA(16, 18, 25, (uint8_t)(200 + 55 * t)));
    float cx = SIDE_W / 2;
    draw_icon(m->logo, cx, 82, 40, m->accent);
    if (t > 0.05f) {
        clip_push(0, 0, w, UI_H);
        text_draw(F_BOLD, 34, SIDE_W - 6, 58, color_alpha(C_TEXT, t), AL_LEFT, m->title);
        if (m->subtitle[0]) text_draw_clip(F_REG, 22, SIDE_W - 6, 104, SIDE_OPEN_W - SIDE_W - 20, color_alpha(C_TEXT3, t), AL_LEFT, m->subtitle);
        clip_pop();
    }
    float item_h = 72, top = 190;
    clip_push(0, top - 10, w, UI_H - top - 30);
    float y = top - m->scroll;
    for (int i = 0; i < m->n; i++) {
        SideItem *it = &m->items[i];
        if (it->gap_before) y += 28;
        bool f = m->open && m->sel == i;
        bool act = i == m->active;
        float pw = (w - 36);
        if (f) fill_rrect(18, y, pw, 62, 16, C_FOCUS);
        else if (act) fill_rrect(18, y, pw, 62, 16, color_alpha(m->accent, 0.28f + 0.1f * t));
        Color c = f ? RGB(10, 10, 10) : act ? C_TEXT : (m->open ? C_TEXT : C_TEXT2);
        draw_icon(it->icon, cx, y + 31, 28, c);
        if (t > 0.05f) {
            clip_push(SIDE_W - 10, y, w - SIDE_W - 10, 62);
            text_draw_clip(F_SEMI, 26, SIDE_W - 6, y + 15, SIDE_OPEN_W - SIDE_W - 40, color_alpha(c, t), AL_LEFT, it->label);
            clip_pop();
        }
        y += item_h;
    }
    clip_pop();
}

bool side_input(SideMenu *m, const InputEvent *ev, int *activated) {
    *activated = -1;
    if (!m->open) return false;
    switch (ev->a) {
    case A_UP: if (m->sel > 0) m->sel--; return true;
    case A_DOWN: if (m->sel < m->n - 1) m->sel++; return true;
    case A_RIGHT: case A_BACK: m->open = false; return true;
    case A_OK:
        *activated = m->sel;
        m->open = false;
        return true;
    case A_HOME: m->open = false; return false;
    default: return true;
    }
}

/* ================= Ratings ================= */
float draw_ratings(float x, float y, int rt_critic, int rt_audience, int imdb10) {
    char buf[32];
    if (imdb10 >= 0) {
        fill_rrect(x, y, 78, 40, 8, RGB(245, 197, 24));
        text_draw(F_BOLD, 22, x + 39, y + 7, RGB(0, 0, 0), AL_CENTER, "IMDb");
        snprintf(buf, sizeof buf, "%d.%d", imdb10 / 10, imdb10 % 10);
        x += 90;
        x += text_draw(F_SEMI, 28, x, y + 3, C_TEXT, AL_LEFT, buf) + 36;
    }
    if (rt_critic >= 0) {
        bool fresh = rt_critic >= 60;
        float cx = x + 20, cy = y + 22;
        if (fresh) {
            fill_circle(cx, cy, 17, RGB(250, 50, 10));
            fill_tri(cx - 8, cy - 18, cx + 8, cy - 18, cx, cy - 8, RGB(0, 160, 60));
        } else {
            fill_circle(cx, cy, 16, RGB(10, 190, 80));
            fill_circle(cx - 12, cy - 8, 6, RGB(10, 190, 80));
            fill_circle(cx + 13, cy + 7, 6, RGB(10, 190, 80));
        }
        snprintf(buf, sizeof buf, "%d%%", rt_critic);
        x += 48;
        x += text_draw(F_SEMI, 28, x, y + 3, C_TEXT, AL_LEFT, buf) + 36;
    }
    if (rt_audience >= 0) {
        /* popcorn-spand */
        float cx = x + 20;
        fill_rrect(cx - 13, y + 18, 26, 22, 4, rt_audience >= 60 ? RGB(250, 50, 10) : RGB(120, 120, 120));
        for (int i = 0; i < 3; i++) fill_circle(cx - 9 + i * 9, y + 14, 7, RGB(255, 236, 170));
        snprintf(buf, sizeof buf, "%d%%", rt_audience);
        x += 48;
        x += text_draw(F_SEMI, 28, x, y + 3, C_TEXT, AL_LEFT, buf) + 36;
    }
    return x;
}
