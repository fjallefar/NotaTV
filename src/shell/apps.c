/* NotaTV shell - the app register: Settings, the App Store and the app packages
 *
 * Everything on the home screen is an app (NotaApp in tv.h). The home screen, Settings → Apps, the App Store and
 * sign-out all work from this register. Only Settings and the App Store are part of tvshell itself; every other app
 * is a package (src/apps).
 *
 * App packages: a folder per app, named after its id, with
 *   manifest.json   {"id": "twitch", "version": "1.0.0", "api": 2, "name": ..., "icon": ..., ...}
 *                   (the version here is the app's version; src/apps/Makefile gives it to the code as APP_VERSION)
 *   app.so          exports `const NotaApp nota_app`
 *   lang/<code>.po  the app's own translations (optional)
 * NotaTV comes without apps: they are downloaded from the App Store (appcat.c) into <data>/apps-pkg/<id>/ and
 * loaded from there at start, or right after they are downloaded (apps_load_pkg). For development, $TVSHELL_APPS
 * (folders separated by ':') is looked in as well, e.g. src/apps/out.
 * A package built for another NOTA_API is skipped (it must be rebuilt against this tv.h).
 *
 * Installed or not is the setting app_<id> (the same key the old "show on home screen" used, so nothing changes
 * for existing boxes). A package is never unloaded while tvshell runs: an uninstalled app's code stays mapped until
 * the next start, so nothing in flight (jobs, threads) can call into freed code. */
#define _GNU_SOURCE /* dl_iterate_phdr */
#include "tv.h"
#include "vendor/cJSON.h"
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


/* ---------- the system's own apps ---------- */
const NotaApp app_appstore = {
    NOTA_API, "appstore", "App Store", N_("Install and uninstall apps"), TV_VERSION, 900, APP_SYSTEM,
    ICON_CHECK, {16, 150, 140, 255}, {8, 92, 96, 255}, {255, 255, 255, 255},
    .open = appstore_screen,
};
const NotaApp app_settings = {
    NOTA_API, "settings", N_("Settings"), N_("Network, picture, sound, HDMI-CEC and system"), TV_VERSION, 1000, APP_SYSTEM,
    ICON_GEAR, {98, 106, 125, 255}, {58, 64, 80, 255}, {255, 255, 255, 255},
    .open = settings_screen,
};

#define MAX_APPS 64
typedef struct {
    const NotaApp *a;
    char pkg[256];   /* package folder ("" = built in) */
    char data[300];  /* <data>/apps/<id> */
    char ver[24];    /* the version that runs: from manifest.json (app.so of older packages has "1.0.0" fixed) */
    char next[24];   /* a newer version downloaded while this one ran: used after a restart ("" = none) */
    bool inited;
} Entry;
static Entry reg[MAX_APPS];
static int n_reg;
static int gen; /* counted up when apps are loaded, installed or uninstalled */
int apps_gen(void) { return gen; }

static Entry *entry(const NotaApp *a) {
    for (int i = 0; i < n_reg; i++) if (reg[i].a == a) return &reg[i];
    return NULL;
}
const NotaApp *app_find(const char *id) {
    for (int i = 0; i < n_reg; i++) if (!strcmp(reg[i].a->id, id)) return reg[i].a;
    return NULL;
}

static bool id_ok(const char *id) {
    if (!id || !*id || strlen(id) > 32) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return false;
    return true;
}

static void add(const NotaApp *a, const char *pkg, const char *ver) {
    if (n_reg >= MAX_APPS || !id_ok(a->id) || app_find(a->id)) return;
    Entry *e = &reg[n_reg++];
    memset(e, 0, sizeof *e);
    e->a = a;
    snprintf(e->pkg, sizeof e->pkg, "%s", pkg ? pkg : "");
    snprintf(e->ver, sizeof e->ver, "%s", ver && *ver ? ver : a->version ? a->version : "");
    snprintf(e->data, sizeof e->data, "%s/apps/%s", data_dir(), a->id);
    /* sorted by order (stable: the same order keeps the order they were added in) */
    for (int i = n_reg - 1; i > 0 && reg[i - 1].a->order > reg[i].a->order; i--) {
        Entry t = reg[i]; reg[i] = reg[i - 1]; reg[i - 1] = t;
    }
}

/* apps that got a new id (the id is English, see src/apps/README.md). At start, before the packages are loaded,
   a box that has the old one is moved over: installed or not (app_<id>), the data folder, and the old package is
   deleted. The App Store then downloads the new package (appcat.c: installed but missing). The app's own cfg keys
   keep their names. */
static const struct { const char *from, *to; } RENAMED_APPS[] = {
    {"elpris", "electricity"}, /* 1.0.16: Electricity price */
};
#define N_RENAMED (int)(sizeof RENAMED_APPS / sizeof RENAMED_APPS[0])
static bool renamed_from(const char *id) {
    for (int i = 0; i < N_RENAMED; i++) if (id && !strcmp(id, RENAMED_APPS[i].from)) return true;
    return false;
}
static void rename_apps(void) {
    for (int i = 0; i < N_RENAMED; i++) {
        const char *from = RENAMED_APPS[i].from, *to = RENAMED_APPS[i].to;
        char ko[48], kn[48], po[400], dn[400], dold[400];
        snprintf(ko, sizeof ko, "app_%s", from);
        snprintf(kn, sizeof kn, "app_%s", to);
        const char *v = cfg_get(ko, "");
        if (*v) {
            if (!*cfg_get(kn, "")) cfg_set(kn, v);
            cfg_unset(ko);
            SDL_Log("app %s is now %s (app_%s=%s)", from, to, to, cfg_get(kn, ""));
        }
        snprintf(po, sizeof po, "%s/%s", apps_pkg_root(), from);
        struct stat st;
        if (stat(po, &st) == 0) run_cmd("rm -rf '%s'", po);
        snprintf(dold, sizeof dold, "%s/apps/%s", data_dir(), from);
        snprintf(dn, sizeof dn, "%s/apps/%s", data_dir(), to);
        if (stat(dold, &st) == 0 && stat(dn, &st) != 0 && rename(dold, dn) != 0) SDL_Log("app %s: could not move %s", from, dold);
    }
}

/* ---------- crash guard: an app that crashes at every start must not lock the box ----------
 * tvshell is restarted by init when it crashes, so an app that crashes while loading, in init, in tick or in its
 * home row would crash it again and again (apps update on their own, so on every box at once).
 * A crash signal is traced to the app whose code is on the crashing stack (or that is being loaded / initialised)
 * and its id is appended to <data>/app-crashes, which is deleted once tvshell has run for a minute. When the same
 * app is behind CRASH_LIMIT crashes in a row (so the last one within a minute of start), it is skipped from the next
 * start (<data>/app-skip: "<id> <version>" per app) and a message says so. That is before the update rollback in
 * `session` (4 starts).
 * It stays installed: a newer version from the App Store is loaded again, and installing it again in the App Store
 * tries the same version once more. */
#define CRASH_LIMIT 2
static struct { uintptr_t lo, hi; char id[33]; } code_rng[MAX_APPS];
static volatile int n_rng;
static const char *volatile loading_id; /* the app being dlopen'ed or initialised (main thread) */
static char crash_path[320], skip_path[320];
#define MAX_SKIP 8
/* skipped apps; ver "" = found in app-crashes at this start (the version is filled in when its package is seen) */
static struct { char id[33], ver[24]; } skips[MAX_SKIP];
static int n_skip;
static char skipped_name[64]; /* shown as a message shortly after start */

/* the executable segments of the loaded object that contains the address probe */
struct rng_find { uintptr_t probe, lo, hi; };
static int rng_cb(struct dl_phdr_info *info, size_t sz, void *ud) {
    (void)sz;
    struct rng_find *f = ud;
    bool mine = false;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        uintptr_t a = info->dlpi_addr + p->p_vaddr;
        if (p->p_type == PT_LOAD && f->probe >= a && f->probe < a + p->p_memsz) mine = true;
    }
    if (!mine) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        uintptr_t a = info->dlpi_addr + p->p_vaddr;
        if (!f->lo || a < f->lo) f->lo = a;
        if (a + p->p_memsz > f->hi) f->hi = a + p->p_memsz;
    }
    return 1;
}
static void note_code(const NotaApp *a) {
    struct rng_find f = {(uintptr_t)a, 0, 0};
    dl_iterate_phdr(rng_cb, &f);
    int i = n_rng;
    if (!f.hi || i >= MAX_APPS) return;
    code_rng[i].lo = f.lo;
    code_rng[i].hi = f.hi;
    snprintf(code_rng[i].id, sizeof code_rng[i].id, "%s", a->id);
    n_rng = i + 1; /* only now visible to the signal handler */
}

static const char *crash_owner(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    for (int i = 0; i < n; i++) {
        uintptr_t pc = (uintptr_t)bt[i];
        for (int j = 0; j < n_rng; j++)
            if (pc >= code_rng[j].lo && pc < code_rng[j].hi) return code_rng[j].id;
    }
    return loading_id;
}
static void crash_handler(int sig) {
    const char *id = crash_owner();
    if (id && crash_path[0]) {
        int fd = open(crash_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd >= 0) {
            if (write(fd, id, strlen(id)) < 0 || write(fd, "\n", 1) < 0) {}
            close(fd);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static int skip_find(const char *id) {
    for (int i = 0; i < n_skip; i++) if (!strcmp(skips[i].id, id)) return i;
    return -1;
}
static void skip_save(void) {
    if (!n_skip) { unlink(skip_path); return; }
    FILE *f = fopen(skip_path, "w");
    if (!f) return;
    for (int i = 0; i < n_skip; i++) fprintf(f, "%s %s\n", skips[i].id, skips[i].ver[0] ? skips[i].ver : "?");
    fclose(f);
}
static void skip_clear(const char *id) {
    int i = skip_find(id);
    if (i < 0) return;
    skips[i] = skips[--n_skip];
    skip_save();
}

static void crash_guard_init(void) {
    snprintf(crash_path, sizeof crash_path, "%s/app-crashes", data_dir());
    snprintf(skip_path, sizeof skip_path, "%s/app-skip", data_dir());
    void *warm[2];
    backtrace(warm, 2); /* loads the unwinder now, not inside the signal handler */
    static char alt[64 * 1024]; /* the handler also runs after a stack overflow */
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        struct sigaction old, sa;
        if (sigaction(sigs[i], NULL, &old) == 0 && old.sa_handler != SIG_DFL) continue; /* e.g. AddressSanitizer */
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = crash_handler;
        sa.sa_flags = SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(sigs[i], &sa, NULL);
    }
    /* the apps skipped earlier ("<id> <version>" per line) */
    char *s = file_read_all(skip_path, 4096), *save = NULL;
    for (char *l = s ? strtok_r(s, "\n", &save) : NULL; l && n_skip < MAX_SKIP; l = strtok_r(NULL, "\n", &save)) {
        char id[33] = "", ver[24] = "";
        if (sscanf(l, "%32s %23s", id, ver) >= 1 && id_ok(id) && skip_find(id) < 0) {
            snprintf(skips[n_skip].id, sizeof skips[n_skip].id, "%s", id);
            snprintf(skips[n_skip].ver, sizeof skips[n_skip].ver, "%s", strcmp(ver, "?") ? ver : "");
            n_skip++;
        }
    }
    free(s);
    /* the last crashes: CRASH_LIMIT by the same app in a row -> skip it */
    char *t = file_read_all(crash_path, 1 << 14);
    if (t) {
        char last[33] = "";
        int run = 0;
        save = NULL;
        for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
            run = strncmp(l, last, sizeof last) ? 1 : run + 1;
            snprintf(last, sizeof last, "%s", l);
        }
        free(t);
        if (run >= CRASH_LIMIT && id_ok(last)) {
            int i = skip_find(last);
            if (i < 0 && n_skip < MAX_SKIP) i = n_skip++;
            if (i >= 0) {
                snprintf(skips[i].id, sizeof skips[i].id, "%s", last);
                skips[i].ver[0] = 0;
            }
            unlink(crash_path);
            SDL_Log("app %s: crashed tvshell %d times in a row - it is skipped", last, run);
        }
    }
}

/* true = do not load this package (the same version as the one that crashed) */
static bool crash_skip(const char *id, const char *ver, cJSON *m) {
    int i = skip_find(id);
    if (i < 0) return false;
    ver = ver && *ver ? ver : "?";
    if (!skips[i].ver[0]) { /* just found: remember which version crashed, and say so */
        snprintf(skips[i].ver, sizeof skips[i].ver, "%s", ver);
        skip_save();
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(m, "name"));
        snprintf(skipped_name, sizeof skipped_name, "%s", name && *name ? name : id);
    } else if (strcmp(ver, skips[i].ver)) {
        SDL_Log("app %s: version %s replaces %s, which crashed - loading it again", id, ver, skips[i].ver);
        skip_clear(id);
        return false;
    }
    SDL_Log("app %s %s: skipped (it crashed tvshell, see app-skip)", id, ver);
    return true;
}

bool apps_crash_skipped(const char *id, const char *version) {
    int i = id ? skip_find(id) : -1;
    if (i < 0) return false;
    return !version || !skips[i].ver[0] || !strcmp(version, skips[i].ver);
}

/* one package folder: check the manifest, load app.so and register the app it exports */
static const NotaApp *load_pkg(const char *dir, const char *name) {
    if (renamed_from(name)) { SDL_Log("app %s: renamed - the old package is skipped (%s)", name, dir); return NULL; }
    const NotaApp *got = NULL;
    char path[512];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    char *txt = file_read_all(path, 1 << 16);
    cJSON *m = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    if (!m) { SDL_Log("app %s: no manifest.json", dir); return NULL; }
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(m, "id"));
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(m, "version"));
    int api = cJSON_GetNumberValue(cJSON_GetObjectItem(m, "api"));
    if (!id_ok(id) || strcmp(id, name)) SDL_Log("app %s: the id in manifest.json must be the folder name", dir);
    else if (api != NOTA_API) SDL_Log("app %s: built for app API %d, this NotaTV has %d - skipped", id, api, NOTA_API);
    else if (app_find(id)) SDL_Log("app %s: already loaded (%s skipped)", id, dir);
    else if (crash_skip(id, ver, m)) {}
    else {
        snprintf(path, sizeof path, "%s/app.so", dir);
        loading_id = id; /* a crash in its constructors counts as this app's */
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        const NotaApp *a = h ? dlsym(h, "nota_app") : NULL;
        loading_id = NULL;
        if (h && a) note_code(a);
        if (!h) SDL_Log("app %s: %s", id, dlerror());
        else if (!a || a->api != NOTA_API || !a->id || strcmp(a->id, id)) {
            SDL_Log("app %s: app.so has no matching nota_app - skipped", id);
            dlclose(h);
        } else {
            i18n_load_extra(dir);
            add(a, dir, ver);
            got = a;
            gen++;
            app_art_changed(); /* its own icon.png may be in the package */
            SDL_Log("app %s %s loaded from %s", id, app_version(a), dir);
        }
    }
    cJSON_Delete(m);
    return got;
}

const char *apps_pkg_root(void) {
    static char p[300];
    if (!p[0]) snprintf(p, sizeof p, "%s/apps-pkg", data_dir());
    return p;
}

/* a package that was just downloaded (<apps-pkg>/<id>); NULL if it cannot be loaded */
const NotaApp *apps_load_pkg(const char *id) {
    if (!id_ok(id)) return NULL;
    skip_clear(id); /* installed again in the App Store after it crashed: try it once more */
    char dir[400];
    snprintf(dir, sizeof dir, "%s/%s", apps_pkg_root(), id);
    return load_pkg(dir, id);
}

static void scan(const char *root) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char dir[400];
        snprintf(dir, sizeof dir, "%s/%s", root, e->d_name);
        struct stat st;
        if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) load_pkg(dir, e->d_name);
    }
    closedir(d);
}

void apps_init(void) {
    static const NotaApp *const builtin[] = {&app_appstore, &app_settings};
    for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++) add(builtin[i], NULL, NULL);
    crash_guard_init();
    rename_apps();
    scan(apps_pkg_root()); /* downloaded from the App Store */
    const char *env = getenv("TVSHELL_APPS");
    if (env && *env) {
        char *list = str_dup(env), *save = NULL;
        for (char *p = strtok_r(list, ":", &save); p; p = strtok_r(NULL, ":", &save)) scan(p);
        free(list);
    }
    for (int i = 0; i < n_reg; i++)
        if (app_installed(reg[i].a) && reg[i].a->init && !reg[i].inited) {
            reg[i].inited = true;
            loading_id = reg[i].a->id;
            reg[i].a->init();
            loading_id = NULL;
        }
}

int app_count(void) { return n_reg; }
const NotaApp *app_at(int i) { return i >= 0 && i < n_reg ? reg[i].a : NULL; }

static void key_of(const NotaApp *a, char *k, size_t n) { snprintf(k, n, "app_%s", a->id); }

bool app_installed(const NotaApp *a) {
    if (!a) return false;
    if (a->flags & APP_SYSTEM) return true;
    char k[48];
    key_of(a, k, sizeof k);
    return cfg_int(k, (a->flags & APP_DEFAULT_OFF) ? 0 : 1) != 0;
}
bool app_on(const char *id) { return app_installed(app_find(id)); }
bool app_packaged(const NotaApp *a) { Entry *e = entry(a); return e && e->pkg[0]; }
const char *app_pkg_dir(const NotaApp *a) { Entry *e = entry(a); return e ? e->pkg : ""; }
const char *app_version(const NotaApp *a) { Entry *e = entry(a); return e ? e->ver : ""; }
const char *app_next_version(const NotaApp *a) { Entry *e = entry(a); return e ? e->next : ""; }
void app_set_next_version(const NotaApp *a, const char *ver) {
    Entry *e = entry(a);
    if (e) snprintf(e->next, sizeof e->next, "%s", ver && strcmp(ver, e->ver) ? ver : "");
}

const char *app_data(const NotaApp *a) {
    Entry *e = entry(a);
    if (!e) return data_dir();
    char p[320];
    snprintf(p, sizeof p, "%s/apps", data_dir());
    mkdir(p, 0700);
    mkdir(e->data, 0700);
    return e->data;
}

const void *app_iface(const char *id) {
    const NotaApp *a = app_find(id);
    return a && app_installed(a) ? a->iface : NULL;
}

bool apps_inbox(const char *name, const char *val) {
    for (int i = 0; i < n_reg; i++)
        if (reg[i].a->inbox && app_installed(reg[i].a) && reg[i].a->inbox(name, val)) return true;
    return false;
}

void apps_tick(void) {
    static bool settled;
    double now = app_time();
    if (!settled && now > 60) { settled = true; unlink(crash_path); } /* ran a minute: earlier crashes do not count */
    if (skipped_name[0] && now > 3) {
        app_toast(T("%s stopped working and was turned off. Install it again in the App Store to try again"), skipped_name);
        skipped_name[0] = 0;
    }
    for (int i = 0; i < n_reg; i++)
        if (reg[i].a->tick && app_installed(reg[i].a)) reg[i].a->tick();
}

void app_install(const NotaApp *a) {
    Entry *e = entry(a);
    if (!e || (a->flags & APP_SYSTEM)) return;
    char k[48];
    key_of(a, k, sizeof k);
    cfg_set_int(k, 1);
    gen++;
    if (a->init && !e->inited) { e->inited = true; loading_id = a->id; a->init(); loading_id = NULL; }
    if (a->apply) a->apply();
}

/* uninstall: background services stop, everything the app saved is deleted (its sign-out, its data folder and
   its own settings), and it disappears from the home screen */
void app_uninstall(const NotaApp *a) {
    Entry *e = entry(a);
    if (!e || (a->flags & APP_SYSTEM)) return;
    char k[48];
    key_of(a, k, sizeof k);
    cfg_set_int(k, 0);
    gen++;
    if (a->apply) a->apply();
    if (a->forget) a->forget();
    home_hidden_forget(a->id);
    for (int i = 0; a->settings && i < a->n_settings; i++) {
        const char *key = a->settings[i].key;
        if (!key || a->settings[i].type == IT_INFO) continue;
        bool shared = false; /* e.g. "Home screen (Plex and Jellyfin)": kept while another installed app uses it */
        for (int j = 0; j < n_reg && !shared; j++) {
            const NotaApp *o = reg[j].a;
            if (o == a || !app_installed(o)) continue;
            for (int m = 0; o->settings && m < o->n_settings; m++)
                if (o->settings[m].key && !strcmp(o->settings[m].key, key)) { shared = true; break; }
        }
        if (!shared) cfg_unset(key);
    }
    struct stat st;
    if (stat(e->data, &st) == 0) { wipe_dir(e->data); rmdir(e->data); }
    e->next[0] = 0;
    wipe_session_traces();
    /* the downloaded package goes too (deleted, never overwritten: its app.so stays mapped until the next start) */
    size_t rl = strlen(apps_pkg_root());
    if (!strncmp(e->pkg, apps_pkg_root(), rl) && e->pkg[rl] == '/') {
        char *cmd = str_printf("%s/apppkg remove '%s' '%s' >/dev/null 2>&1", helper_path(), apps_pkg_root(), a->id);
        run_cmd("%s", cmd);
        free(cmd);
    }
    app_art_changed();
}
