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
#include "tv.h"
#include "vendor/cJSON.h"
#include <dirent.h>
#include <dlfcn.h>
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

/* one package folder: check the manifest, load app.so and register the app it exports */
static const NotaApp *load_pkg(const char *dir, const char *name) {
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
    else {
        snprintf(path, sizeof path, "%s/app.so", dir);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        const NotaApp *a = h ? dlsym(h, "nota_app") : NULL;
        if (!h) SDL_Log("app %s: %s", id, dlerror());
        else if (!a || a->api != NOTA_API || !a->id || strcmp(a->id, id)) {
            SDL_Log("app %s: app.so has no matching nota_app - skipped", id);
            dlclose(h);
        } else {
            i18n_load_extra(dir);
            add(a, dir, ver);
            got = a;
            gen++;
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
    scan(apps_pkg_root()); /* downloaded from the App Store */
    const char *env = getenv("TVSHELL_APPS");
    if (env && *env) {
        char *list = str_dup(env), *save = NULL;
        for (char *p = strtok_r(list, ":", &save); p; p = strtok_r(NULL, ":", &save)) scan(p);
        free(list);
    }
    for (int i = 0; i < n_reg; i++)
        if (app_installed(reg[i].a) && reg[i].a->init && !reg[i].inited) { reg[i].inited = true; reg[i].a->init(); }
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
    if (a->init && !e->inited) { e->inited = true; a->init(); }
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
}
