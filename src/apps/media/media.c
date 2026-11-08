/* NotaTV app package "media" - Media: file browser for USB drives, network drives and a local folder */
#include "media.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct { char *name; bool dir; long long size; } Entry;

typedef struct {
    char path[2048];
    Entry *e;
    int n, sel;
    float scroll;
    bool root;
    bool loading;
} Media;

static const char *video_ext[] = {"mkv", "mp4", "m4v", "avi", "mov", "ts", "m2ts", "webm", "wmv", "mpg", "mpeg", "flv", "iso", NULL};
static const char *audio_ext[] = {"mp3", "flac", "m4a", "aac", "ogg", "opus", "wav", "wma", "alac", "dsf", NULL};
static const char *image_ext[] = {"jpg", "jpeg", "png", "gif", "webp", "bmp", NULL};

static bool has_ext(const char *name, const char **list) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (int i = 0; list[i]; i++)
        if (!strcasecmp(dot + 1, list[i])) return true;
    return false;
}
static bool is_media(const char *n) { return has_ext(n, video_ext) || has_ext(n, audio_ext) || has_ext(n, image_ext); }

static int ent_cmp(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void free_entries(Media *m) {
    for (int i = 0; i < m->n; i++) free(m->e[i].name);
    free(m->e);
    m->e = NULL;
    m->n = 0;
}

static Media *live;
static unsigned load_gen; /* a scan's result is used only while it is still the newest */

typedef struct {
    unsigned gen;
    char path[2048];
    bool root;
    bool announce;
    Entry *e;
    int n;
    bool failed;
} Scan;

static void scan_work(void *arg) {
    Scan *s = arg;
    if (s->root) {
        /* root level: mounted drives (automount runs in the background and the list is updated continuously) */
        DIR *d = opendir("/media");
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            s->e = realloc(s->e, sizeof(Entry) * (s->n + 1));
            s->e[s->n++] = (Entry){str_dup(de->d_name), true, 0};
        }
        if (d) closedir(d);
    } else {
        DIR *d = opendir(s->path);
        if (!d) { s->failed = true; return; }
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char full[4096];
            snprintf(full, sizeof full, "%s/%s", s->path, de->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;
            bool dir = S_ISDIR(st.st_mode);
            if (!dir && !is_media(de->d_name)) continue;
            s->e = realloc(s->e, sizeof(Entry) * (s->n + 1));
            s->e[s->n++] = (Entry){str_dup(de->d_name), dir, (long long)st.st_size};
        }
        closedir(d);
    }
    qsort(s->e, s->n, sizeof(Entry), ent_cmp);
}

static void scan_done(void *arg) {
    Scan *s = arg;
    Media *m = live;
    if (m && s->gen == load_gen) {
        if (s->failed) {
            app_toast(T("Cannot open the folder"));
        } else {
            free_entries(m);
            m->e = s->e;
            m->n = s->n;
            s->e = NULL;
            s->n = 0;
            m->sel = m->sel < m->n ? m->sel : 0;
            if (s->announce) app_toast(T("Refreshed"));
        }
        m->loading = false;
    }
    for (int i = 0; i < s->n; i++) free(s->e[i].name);
    free(s->e);
    free(s);
}

static void load_dir(Media *m, bool announce) {
    Scan *s = calloc(1, sizeof *s);
    if (!s) return;
    s->gen = ++load_gen;
    s->root = m->root;
    s->announce = announce;
    snprintf(s->path, sizeof s->path, "%s", m->path);
    m->loading = true;
    job_submit_q(JQ_SLOW, scan_work, scan_done, s);
}

static void media_enter(Screen *s) {
    Media *m = s->st;
    live = m;
    if (m->root) run_cmd("%s/automount >/dev/null 2>&1 &", helper_path());
    load_dir(m, false);
}

static void media_resume(Screen *s) {
    Media *m = s->st;
    if (live == m) return; /* still the same screen on top (e.g. only a menu was open) */
    live = m;
    if (m->loading) load_dir(m, false); /* the scan was dropped while another folder was on top */
}

static int count_media_dirs(void) {
    int n = 0;
    DIR *d = opendir("/media");
    struct dirent *de;
    while (d && (de = readdir(d))) if (de->d_name[0] != '.') n++;
    if (d) closedir(d);
    return n;
}

static void media_destroy(Screen *s) {
    Media *m = s->st;
    if (live == m) { live = NULL; load_gen++; }
    free_entries(m);
    free(m);
    free(s);
}

static void media_update(Screen *s, float dt) {
    Media *m = s->st;
    static double last_scan;
    if (m->root && !m->loading && app_time() - last_scan > 2) {
        last_scan = app_time();
        if (count_media_dirs() != m->n) load_dir(m, false);
        static double last_mount;
        if (app_time() - last_mount > 6) { last_mount = app_time(); run_cmd("%s/automount >/dev/null 2>&1 &", helper_path()); }
    }
    float target = m->sel * 84 - 84 * 4;
    if (target < 0) target = 0;
    m->scroll = anim_to(m->scroll, target, dt, 14);
}

static void media_draw(Screen *s) {
    Media *m = s->st;
    draw_background();
    draw_topbar(T("Media"));
    text_draw_clip(F_REG, 24, 96, 118, 1500, C_TEXT3, AL_LEFT, m->root ? T("Drives") : m->path);
    if (m->loading) draw_spinner(UI_W - 48, 118, 14, C_TEXT2);
    if (m->n == 0) {
        if (m->loading) {
            draw_spinner(UI_W / 2, UI_H / 2 - 30, 30, C_TEXT);
            text_draw(F_REG, 24, UI_W / 2, UI_H / 2 + 20, C_TEXT3, AL_CENTER, T("Loading…"));
            return;
        }
        draw_empty(m->root ? T("No drives found") : T("Empty folder"),
                   m->root ? T("Plug a USB drive into the box, or add a network drive under Settings → Cast and sharing.") :
                             T("There are no movies, music or pictures in this folder."));
        draw_keybar(96, UI_H - 62, NULL, NULL, NULL, NULL, T("Refresh"));
        return;
    }
    clip_push(0, 160, UI_W, UI_H - 220);
    for (int i = 0; i < m->n; i++) {
        float y = 170 + i * 84 - m->scroll;
        if (y < 90 || y > UI_H) continue;
        Entry *e = &m->e[i];
        bool f = i == m->sel && !keybar_focused();
        if (f) fill_rrect(96, y, UI_W - 192, 74, 14, C_FOCUS);
        Color tc = f ? RGB(10, 10, 10) : C_TEXT;
        IconId ic = m->root ? ICON_USB : e->dir ? ICON_FOLDER : has_ext(e->name, audio_ext) ? ICON_MUSIC : ICON_FILM;
        draw_icon(ic, 146, y + 37, 34, f ? RGB(10, 10, 10) : C_TEXT2);
        text_draw_clip(F_SEMI, 28, 196, y + 20, 1300, tc, AL_LEFT, e->name);
        if (!e->dir) {
            char sz[32];
            double mb = e->size / 1048576.0;
            if (mb > 1024) snprintf(sz, sizeof sz, "%.1f GB", mb / 1024);
            else snprintf(sz, sizeof sz, "%.0f MB", mb);
            text_draw(F_REG, 24, UI_W - 130, y + 23, f ? RGB(60, 60, 60) : C_TEXT3, AL_RIGHT, sz);
        }
    }
    clip_pop();
    draw_keybar(96, UI_H - 62, NULL, NULL, NULL, NULL, T("Refresh")); /* ↓ below the last row (also the Menu key) */
}

static Screen *media_at(const char *path);

static void play_entry(Media *m, int idx) {
    Entry *e = &m->e[idx];
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", m->path, e->name);
    /* a name with a line break cannot be a line in the playlist: such a file is played on its own */
    bool plain = !strpbrk(e->name, "\r\n");
    if (plain && (has_ext(e->name, audio_ext) || has_ext(e->name, image_ext))) {
        /* play the whole folder as a playlist from the chosen file */
        const char **kind = has_ext(e->name, audio_ext) ? audio_ext : image_ext;
        const char *pl = "/tmp/notatv-media.m3u";
        FILE *f = fopen(pl, "w");
        int start = 0, k = 0;
        for (int i = 0; f && i < m->n; i++) {
            if (m->e[i].dir || !has_ext(m->e[i].name, kind) || strpbrk(m->e[i].name, "\r\n")) continue;
            if (i == idx) start = k;
            fprintf(f, "%s/%s\n", m->path, m->e[i].name);
            k++;
        }
        if (f) fclose(f);
        PlayRequest rq = {0};
        rq.url = pl;
        rq.title = NULL;
        rq.playlist_start = start;
        static const char *img_extra[] = {"--image-display-duration=6", NULL};
        rq.extra = kind == image_ext ? img_extra : NULL;
        player_run(&rq);
        return;
    }
    PlayRequest rq = {0};
    rq.url = full;
    rq.title = e->name;
    player_run(&rq);
}

static void media_menu_cb(void *ud, int idx) { /* Red = menu */
    if (idx == 0) load_dir(ud, true);
}

static bool media_input(Screen *s, const InputEvent *ev) {
    Media *m = s->st;
    switch (ev->a) {
    case A_UP: if (m->sel > 0) m->sel--; return true;
    case A_DOWN: if (m->sel < m->n - 1) { m->sel++; return true; } return false; /* below the last: the bar */
    case A_CHUP: m->sel = m->sel > 8 ? m->sel - 8 : 0; return true;
    case A_CHDOWN: m->sel = m->sel + 8 < m->n ? m->sel + 8 : (m->n ? m->n - 1 : 0); return true;
    case A_OK: case A_RIGHT: case A_PLAYPAUSE:
        if (m->n == 0) return true;
        if (m->root || m->e[m->sel].dir) {
            char full[4096];
            if (m->root) snprintf(full, sizeof full, "/media/%s", m->e[m->sel].name);
            else snprintf(full, sizeof full, "%s/%s", m->path, m->e[m->sel].name);
            app_push(media_at(full));
        } else if (ev->a != A_RIGHT) play_entry(m, m->sel);
        return true;
    case A_LEFT: app_pop(); return true;
    case A_MENU: media_menu_cb(m, 0); return true; /* the only choice: refresh right away */
    default: return false;
    }
}

static Screen *media_at(const char *path) {
    Screen *s = calloc(1, sizeof *s);
    Media *m = calloc(1, sizeof *m);
    m->root = path == NULL;
    if (path) snprintf(m->path, sizeof m->path, "%s", path);
    s->name = "media";
    s->st = m;
    s->enter = media_enter;
    s->resume = media_resume;
    s->update = media_update;
    s->draw = media_draw;
    s->input = media_input;
    s->destroy = media_destroy;
    return s;
}

Screen *media_screen(void) { return media_at(NULL); }

/* ---------- the app (see NotaApp in tv.h) ---------- */
const NotaApp nota_app = {
    NOTA_API, "media", N_("Media"), N_("Movies, music and pictures from USB and network"), APP_VERSION, 90, 0,
    ICON_FOLDER, {123, 77, 255, 255}, {75, 42, 179, 255}, {255, 255, 255, 255},
    .open = media_screen,
};
