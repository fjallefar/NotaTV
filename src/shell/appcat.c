/* NotaTV shell - the App Store catalog: which apps can be downloaded, and installing them
 *
 * NotaTV comes without apps. The catalog is a JSON file on the GitHub release "apps" (made by
 * scripts/apps-catalog.py when a release is built): https://github.com/<repo>/releases/download/apps/apps-api<N>.json,
 * one per app API version, so a box only sees packages built for its own tv.h. <repo> is the one the system
 * updates come from (cfg update_repo or /etc/notatv-update-repo); cfg apps_url or $TVSHELL_APPCAT_URL replace the
 * whole address (tests: file://...).
 *
 * Installing goes through one queue (the App Store, the setup guide and the move below all use it): the helper
 * "apppkg install" downloads the package, checks its SHA256 from the catalog and unpacks it into <data>/apps-pkg/<id>;
 * then the app is loaded (apps_load_pkg) and installed. Updating an app that is already loaded only takes effect
 * after a restart of the user interface (a loaded app.so is never replaced while tvshell runs).
 *
 * Boxes from before the App Store (NotaTV 1.0.x had the apps built in) get the apps they had - every app not turned
 * off with app_<id>=0 - downloaded once by themselves (cfg apps_migrated). */
#include "tv.h"
#include "vendor/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CAT 64
#define MAX_QUEUE 32

static CatApp cat[MAX_CAT];
static int n_cat;
static CatState state = CAT_NONE;
static int cat_gen; /* counted up for every catalog read */
static char cat_err[160];
static bool restart_needed;

IconId icon_by_name(const char *n) {
    static const struct { const char *n; IconId id; } map[] = {
        {"tv", ICON_TV}, {"plex", ICON_PLEX}, {"youtube", ICON_YOUTUBE}, {"folder", ICON_FOLDER}, {"gear", ICON_GEAR},
        {"play", ICON_PLAY}, {"wifi", ICON_WIFI}, {"star", ICON_STAR}, {"search", ICON_SEARCH}, {"music", ICON_MUSIC},
        {"film", ICON_FILM}, {"check", ICON_CHECK}, {"lock", ICON_LOCK}, {"clock", ICON_CLOCK}, {"moon", ICON_MOON}, {"power", ICON_POWER}};
    for (size_t i = 0; n && i < sizeof map / sizeof map[0]; i++) if (!strcmp(map[i].n, n)) return map[i].id;
    return ICON_PLAY;
}

/* 1.10.0 > 1.9.2 (numbers per part) */
int version_cmp(const char *a, const char *b) {
    while ((a && *a) || (b && *b)) {
        long x = a && *a ? strtol(a, (char **)&a, 10) : 0, y = b && *b ? strtol(b, (char **)&b, 10) : 0;
        if (x != y) return x < y ? -1 : 1;
        if (a && *a == '.') a++;
        if (b && *b == '.') b++;
        if ((a && *a && (*a < '0' || *a > '9')) || (b && *b && (*b < '0' || *b > '9'))) break;
    }
    return 0;
}

static const char *base_url(char *buf, size_t n) {
    const char *e = getenv("TVSHELL_APPCAT_URL");
    if (e && *e) { snprintf(buf, n, "%s", e); return buf; }
    /* from the settings only over https (the packages are code that runs in tvshell); file:// etc. only via the
       environment for development */
    const char *u = cfg_get("apps_url", "");
    if (!strncmp(u, "https://", 8)) { snprintf(buf, n, "%s", u); return buf; }
    if (*u) SDL_Log("appcat: apps_url skal være https:// - bruger standardbutikken");
    char repo[128] = "";
    snprintf(repo, sizeof repo, "%s", cfg_get("update_repo", ""));
    if (!repo[0]) {
        FILE *f = fopen("/etc/notatv-update-repo", "r");
        if (f) { if (!fgets(repo, sizeof repo, f)) repo[0] = 0; fclose(f); }
    }
    str_trim(repo);
    for (const char *p = repo; *p; p++) /* owner/name only */
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("._-/", *p))) repo[0] = 0;
    if (!repo[0] || !strchr(repo, '/')) return NULL;
    snprintf(buf, n, "https://github.com/%s/releases/download/apps", repo);
    return buf;
}

/* ---------- the catalog ---------- */
CatState appcat_state(void) { return state; }
const char *appcat_error(void) { return cat_err; }
int appcat_count(void) { return state == CAT_OK ? n_cat : 0; }
const CatApp *appcat_at(int i) { return i >= 0 && i < appcat_count() ? &cat[i] : NULL; }
const CatApp *appcat_find(const char *id) {
    for (int i = 0; i < appcat_count(); i++) if (!strcmp(cat[i].id, id)) return &cat[i];
    return NULL;
}

static Color jcolor(cJSON *a) {
    Color c = {255, 255, 255, 255};
    if (cJSON_GetArraySize(a) >= 3) {
        c.r = (uint8_t)cJSON_GetNumberValue(cJSON_GetArrayItem(a, 0));
        c.g = (uint8_t)cJSON_GetNumberValue(cJSON_GetArrayItem(a, 1));
        c.b = (uint8_t)cJSON_GetNumberValue(cJSON_GetArrayItem(a, 2));
    }
    return c;
}
static const char *jstr(cJSON *o, const char *k) { const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k)); return s ? s : ""; }

/* ---------- the apps' own pictures (shown in the App Store before an app is installed) ---------- */
static const char *icon_dir(void) {
    static char p[300];
    if (!p[0]) snprintf(p, sizeof p, "%s/app-icons", data_dir());
    return p;
}
bool appcat_icon_path(const CatApp *c, char *out, size_t n) {
    if (!c || !c->icon_file[0]) return false;
    snprintf(out, n, "%s/%s-%.16s.png", icon_dir(), c->id, c->icon_sha);
    return true;
}
typedef struct { int n; char id[MAX_CAT][40], file[MAX_CAT][96], sha[MAX_CAT][72]; char url[400]; int got; } IconJob;
static bool icons_running;
static void icons_work(void *arg) { /* one after another in the background; a failed one is tried again next catalog */
    IconJob *j = arg;
    char prog[300];
    snprintf(prog, sizeof prog, "%s/apppkg", helper_path());
    for (int i = 0; i < j->n; i++) {
        const char *argv[] = {prog, "icon", j->url, icon_dir(), j->id[i], j->file[i], j->sha[i], NULL}; /* no shell */
        char *out = run_capture_argv(argv);
        if (out && !strncmp(out, "OK", 2)) j->got++;
        else SDL_Log("app-ikon %s: %s", j->id[i], out ? out : "?");
        free(out);
    }
}
static void icons_done(void *arg) {
    IconJob *j = arg;
    if (j->got) app_art_changed();
    icons_running = false;
    free(j);
}
static void icons_fetch(void) {
    if (icons_running) return;
    IconJob *j = calloc(1, sizeof *j);
    if (!j) return;
    if (!base_url(j->url, sizeof j->url)) { free(j); return; }
    for (int i = 0; i < n_cat; i++) {
        char p[400];
        const CatApp *c = &cat[i];
        if (!appcat_icon_path(c, p, sizeof p) || access(p, R_OK) == 0) continue;
        snprintf(j->id[j->n], sizeof j->id[0], "%s", c->id);
        snprintf(j->file[j->n], sizeof j->file[0], "%s", c->icon_file);
        snprintf(j->sha[j->n], sizeof j->sha[0], "%s", c->icon_sha);
        j->n++;
    }
    if (!j->n) { free(j); return; }
    icons_running = true;
    job_submit_q(JQ_SLOW, icons_work, icons_done, j);
}

/* ---------- tools (src/tools): programs from the catalog's "tools", never shown in the store ----------
 * Today only yt-dlp: YouTube changes often and an old yt-dlp stops working, so a newer one comes with the catalog
 * instead of with a system release. It is installed in <data>/tools/<id> (apppkg tool-install, SHA256 from the
 * catalog like a package) when it is newer than both the image's and the one already installed; mediarun runs it. */
#define MAX_TOOLS 4
typedef struct { char id[40], version[24], file[128], sha[72]; } CatTool;
static CatTool tools[MAX_TOOLS];
static int n_tools;
static bool tool_running;
static const char *tools_root(void) {
    static char p[300];
    if (!p[0]) snprintf(p, sizeof p, "%s/tools", data_dir());
    return p;
}
/* the version in a small file / manifest ("" = none) */
static void tool_version(const char *id, bool builtin, char *out, size_t n) {
    out[0] = 0;
    char path[400];
    if (builtin) snprintf(path, sizeof path, "/usr/lib/%s/notatv-version", id);
    else snprintf(path, sizeof path, "%s/%s/manifest.json", tools_root(), id);
    char *txt = file_read_all(path, 1 << 14);
    if (!txt) return;
    if (builtin) { snprintf(out, n, "%s", txt); str_trim(out); }
    else {
        cJSON *m = cJSON_Parse(txt);
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(m, "version"));
        if (v) snprintf(out, n, "%s", v);
        cJSON_Delete(m);
    }
    free(txt);
}
typedef struct { CatTool t; char url[400]; char *out; } ToolJob;
static void tool_work(void *arg) {
    ToolJob *j = arg;
    char prog[300];
    snprintf(prog, sizeof prog, "%s/apppkg", helper_path());
    const char *argv[] = {prog, "tool-install", j->url, tools_root(), j->t.id, j->t.file, j->t.sha, NULL}; /* no shell */
    j->out = run_capture_argv(argv);
}
static void tools_check(void);
static void tool_done(void *arg) {
    ToolJob *j = arg;
    char *out = j->out ? j->out : str_dup("");
    str_trim(out);
    const char *last = strrchr(out, '\n');
    last = last ? last + 1 : out;
    if (!strcmp(last, "OK")) SDL_Log("tool %s %s installed", j->t.id, j->t.version);
    else SDL_Log("tool %s %s: %s (tried again with the next catalog)", j->t.id, j->t.version, last);
    bool ok = !strcmp(last, "OK");
    free(out);
    free(j);
    tool_running = false;
    if (ok) tools_check(); /* the next one, if any */
}
static void tools_check(void) {
    if (tool_running || getenv("TVSHELL_SCRIPT")) return;
    for (int i = 0; i < n_tools; i++) {
        const CatTool *t = &tools[i];
        if (strcmp(t->id, "yt-dlp")) continue; /* mediarun only knows yt-dlp */
        char built[32], have[32];
        tool_version(t->id, true, built, sizeof built);
        tool_version(t->id, false, have, sizeof have);
        if ((built[0] && version_cmp(t->version, built) <= 0) || (have[0] && version_cmp(t->version, have) <= 0)) continue;
        ToolJob *j = calloc(1, sizeof *j);
        if (!j || !base_url(j->url, sizeof j->url)) { free(j); return; }
        j->t = *t;
        tool_running = true;
        SDL_Log("tool %s: %s -> %s", t->id, have[0] ? have : built[0] ? built : "?", t->version);
        job_submit_q(JQ_SLOW, tool_work, tool_done, j);
        return;
    }
}
static void tools_parse(cJSON *arr) {
    n_tools = 0;
    cJSON *a;
    cJSON_ArrayForEach(a, arr) {
        if (n_tools >= MAX_TOOLS) break;
        CatTool *t = &tools[n_tools];
        memset(t, 0, sizeof *t);
        snprintf(t->id, sizeof t->id, "%s", jstr(a, "id"));
        snprintf(t->version, sizeof t->version, "%s", jstr(a, "version"));
        snprintf(t->file, sizeof t->file, "%s", jstr(a, "file"));
        snprintf(t->sha, sizeof t->sha, "%s", jstr(a, "sha256"));
        /* the same checks apppkg makes */
        bool ok = t->id[0] && strlen(t->id) <= 32 && t->version[0] && t->file[0] && strlen(t->sha) == 64;
        for (const char *p = t->id; ok && *p; p++) ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_';
        for (const char *p = t->version; ok && *p; p++) ok = (*p >= '0' && *p <= '9') || *p == '.';
        for (const char *p = t->sha; ok && *p; p++) ok = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
        for (const char *p = t->file; ok && *p; p++) ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("._-", *p);
        if (ok) n_tools++;
    }
}

typedef struct { char *out; } CatJob;
static void cat_work(void *arg) {
    CatJob *j = arg;
    char url[400];
    if (!base_url(url, sizeof url)) { j->out = str_dup("No App Store is set up"); return; }
    char prog[300], api[16];
    snprintf(prog, sizeof prog, "%s/apppkg", helper_path());
    snprintf(api, sizeof api, "%d", NOTA_API);
    const char *argv[] = {prog, "catalog", url, api, NULL}; /* no shell in between */
    j->out = run_capture_argv(argv);
}
static void cat_done(void *arg) {
    CatJob *j = arg;
    cJSON *root = j->out ? cJSON_Parse(j->out) : NULL;
    cJSON *apps = cJSON_GetObjectItem(root, "apps");
    if (!cJSON_IsArray(apps)) {
        snprintf(cat_err, sizeof cat_err, "%s", j->out && *j->out && j->out[0] != '{' ? j->out : "Could not reach the App Store");
        str_trim(cat_err);
        state = CAT_ERROR;
    } else {
        n_cat = 0;
        cat_gen++;
        cJSON *a;
        cJSON_ArrayForEach(a, apps) {
            if (n_cat >= MAX_CAT) break;
            CatApp *c = &cat[n_cat];
            memset(c, 0, sizeof *c);
            snprintf(c->id, sizeof c->id, "%s", jstr(a, "id"));
            snprintf(c->version, sizeof c->version, "%s", jstr(a, "version"));
            snprintf(c->file, sizeof c->file, "%s", jstr(a, "file"));
            snprintf(c->sha, sizeof c->sha, "%s", jstr(a, "sha256"));
            cJSON *tr = cJSON_GetObjectItem(cJSON_GetObjectItem(a, "i18n"), i18n_lang());
            snprintf(c->name, sizeof c->name, "%s", *jstr(tr, "name") ? jstr(tr, "name") : jstr(a, "name"));
            snprintf(c->desc, sizeof c->desc, "%s", *jstr(tr, "desc") ? jstr(tr, "desc") : jstr(a, "desc"));
            c->icon = icon_by_name(jstr(a, "icon"));
            c->c1 = jcolor(cJSON_GetObjectItem(a, "c1"));
            c->c2 = jcolor(cJSON_GetObjectItem(a, "c2"));
            c->fg = jcolor(cJSON_GetObjectItem(a, "fg"));
            c->order = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(a, "order"));
            c->def = cJSON_IsTrue(cJSON_GetObjectItem(a, "default"));
            c->nonfree = cJSON_IsTrue(cJSON_GetObjectItem(a, "nonfree"));
            c->size = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(a, "size"));
            cJSON *is = cJSON_GetObjectItem(a, "installed_size");
            c->isize = cJSON_IsNumber(is) ? (long)is->valuedouble : 0;
            /* what is new: {"summary": "...", "points": ["...", ...]}, translated in i18n.<lang>.changes */
            cJSON *ch = cJSON_GetObjectItem(tr, "changes");
            if (!cJSON_IsObject(ch)) ch = cJSON_GetObjectItem(a, "changes");
            snprintf(c->news, sizeof c->news, "%s", jstr(ch, "summary"));
            cJSON *pt;
            cJSON_ArrayForEach(pt, cJSON_GetObjectItem(ch, "points")) {
                if (c->n_points >= 4) break;
                if (cJSON_IsString(pt) && *pt->valuestring) snprintf(c->points[c->n_points++], sizeof c->points[0], "%s", pt->valuestring);
            }
            /* only what apppkg would take anyway: a plain id, a plain file name and a hex SHA256 */
            bool ok = c->id[0] && strlen(c->id) <= 32 && c->file[0] && strlen(c->sha) == 64;
            for (const char *p = c->id; ok && *p; p++) ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_';
            for (const char *p = c->sha; ok && *p; p++) ok = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
            for (const char *p = c->file; ok && *p; p++) ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("._-", *p);
            /* the app's own picture: a plain .png name and a hex SHA256, like the package */
            snprintf(c->icon_file, sizeof c->icon_file, "%s", jstr(a, "icon_file"));
            snprintf(c->icon_sha, sizeof c->icon_sha, "%s", jstr(a, "icon_sha256"));
            bool iok = strlen(c->icon_sha) == 64 && strlen(c->icon_file) > 4 && !strcmp(c->icon_file + strlen(c->icon_file) - 4, ".png");
            for (const char *p = c->icon_sha; iok && *p; p++) iok = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f');
            for (const char *p = c->icon_file; iok && *p; p++) iok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("._-", *p);
            if (!iok) c->icon_file[0] = c->icon_sha[0] = 0;
            if (ok) n_cat++;
        }
        state = CAT_OK;
        cat_err[0] = 0;
        app_art_changed(); /* pictures from the catalog may now be known */
        icons_fetch();
        tools_parse(cJSON_GetObjectItem(root, "tools"));
        tools_check();
    }
    cJSON_Delete(root);
    free(j->out);
    free(j);
}

void appcat_refresh(void) {
    if (state == CAT_LOADING) return;
    state = CAT_LOADING;
    job_submit_q(JQ_SLOW, cat_work, cat_done, xcalloc(1, sizeof(CatJob)));
}

bool appcat_has_update(const char *id) {
    const NotaApp *a = app_find(id);
    const CatApp *c = appcat_find(id);
    if (!a || !c || !app_installed(a)) return false;
    const char *have = *app_next_version(a) ? app_next_version(a) : app_version(a); /* downloaded, waiting for a restart */
    return version_cmp(c->version, have) > 0;
}
int appcat_updates(void) {
    int n = 0;
    for (int i = 0; i < appcat_count(); i++) if (appcat_has_update(cat[i].id)) n++;
    return n;
}
bool appcat_restart_needed(void) { return restart_needed; }

/* ---------- installing (one at a time) ---------- */
static char queue[MAX_QUEUE][40];
static int q_n, q_pos, q_total, q_done_n; /* q_pos: the one being installed; q_total/q_done_n: progress */
static bool q_running;
static int q_failed; /* since the start (the move below tries again) */

int appcat_pending(const char *id) {
    for (int i = q_pos; i < q_n; i++) if (!strcmp(queue[i], id)) return i == q_pos && q_running ? 2 : 1;
    return 0;
}
bool appcat_busy(int *done, int *total, const char **current) {
    if (done) *done = q_done_n;
    if (total) *total = q_total;
    if (current) {
        const CatApp *c = q_pos < q_n ? appcat_find(queue[q_pos]) : NULL;
        *current = c ? c->name : "";
    }
    return q_pos < q_n;
}

typedef struct { char id[40], url[400], file[128], sha[72]; char *out; } InstJob;
static void inst_work(void *arg) {
    InstJob *j = arg;
    char prog[300];
    snprintf(prog, sizeof prog, "%s/apppkg", helper_path());
    const char *argv[] = {prog, "install", j->url, apps_pkg_root(), j->id, j->file, j->sha, NULL}; /* no shell */
    j->out = run_capture_argv(argv);
}
static void start_next(void);
static void inst_done(void *arg) {
    InstJob *j = arg;
    q_running = false;
    const CatApp *c = appcat_find(j->id);
    char *out = j->out ? j->out : str_dup("");
    str_trim(out);
    const char *last = strrchr(out, '\n');
    last = last ? last + 1 : out;
    if (!strcmp(last, "OK")) {
        const NotaApp *a = app_find(j->id);
        if (a) { /* already loaded (an update, or installed again): the new code is used after a restart */
            if (c) app_set_next_version(a, c->version);
            if (*app_next_version(a)) restart_needed = true;
        } else a = apps_load_pkg(j->id);
        if (a) app_install(a);
        else app_toast(T("%s could not be started on this version of NotaTV"), c ? c->name : j->id);
    } else {
        q_failed++;
        app_toast("%s: %s", c ? c->name : j->id, *last ? tr_msg(last) : T("Could not install the app"));
    }
    free(out);
    free(j);
    q_pos++;
    q_done_n++;
    start_next();
}
static void start_next(void) {
    if (q_running) return;
    if (q_pos >= q_n) { q_n = q_pos = 0; return; } /* empty: q_total/q_done_n stay for the last view */
    const CatApp *c = appcat_find(queue[q_pos]);
    char url[400];
    if (!c || !base_url(url, sizeof url)) { q_pos++; q_done_n++; start_next(); return; }
    InstJob *j = xcalloc(1, sizeof *j);
    snprintf(j->id, sizeof j->id, "%s", c->id);
    snprintf(j->url, sizeof j->url, "%s", url);
    snprintf(j->file, sizeof j->file, "%s", c->file);
    snprintf(j->sha, sizeof j->sha, "%s", c->sha);
    q_running = true;
    job_submit_q(JQ_SLOW, inst_work, inst_done, j);
}

void appcat_install(const char *id) {
    if (!appcat_find(id) || appcat_pending(id) || q_n >= MAX_QUEUE) return;
    if (q_pos >= q_n) { q_total = 0; q_done_n = 0; } /* a new round */
    snprintf(queue[q_n++], sizeof queue[0], "%s", id);
    q_total++;
    start_next();
}

void appcat_update_all(void) {
    for (int i = 0; i < appcat_count(); i++) if (appcat_has_update(cat[i].id)) appcat_install(cat[i].id);
}

/* ---------- boxes from before the App Store ---------- */
static void migrate_tick(void) {
    static int phase; /* 0 = check, 1 = waiting for the catalog, 2 = installing, 3 = done */
    static double retry_at;
    if (phase == 3) return;
    if (phase == 0) {
        if (!cfg_int("setup_done", 0) || cfg_int("apps_migrated", 0) ||
            (getenv("TVSHELL_SCRIPT") && !getenv("TVSHELL_MIGRATE_TEST"))) { phase = 3; return; }
        for (int i = 0; i < app_count(); i++) /* already has App Store apps */
            if (app_packaged(app_at(i)) && !strncmp(app_pkg_dir(app_at(i)), apps_pkg_root(), strlen(apps_pkg_root()))) {
                cfg_set_int("apps_migrated", 1);
                phase = 3;
                return;
            }
        phase = 1;
    }
    if (phase == 1) {
        if (app_time() < retry_at) return;
        if (state == CAT_ERROR && retry_at == 0) { retry_at = app_time() + 300; return; }
        if (state != CAT_OK && state != CAT_LOADING) { retry_at = 0; appcat_refresh(); return; }
        if (state != CAT_OK) return;
        q_failed = 0;
        int n = 0;
        for (int i = 0; i < appcat_count(); i++) {
            const CatApp *c = appcat_at(i);
            char k[48];
            snprintf(k, sizeof k, "app_%s", c->id);
            const char *v = cfg_get(k, NULL);
            if ((v && *v ? atoi(v) != 0 : c->def) && !app_installed(app_find(c->id)) && !apps_crash_skipped(c->id, c->version)) {
                appcat_install(c->id);
                n++;
            }
        }
        if (n) app_toast(T("Installing your apps from the App Store…"));
        phase = 2;
    }
    if (phase == 2 && !appcat_busy(NULL, NULL, NULL)) {
        if (q_failed) { phase = 1; retry_at = app_time() + 300; return; } /* no network meanwhile: try again later */
        cfg_set_int("apps_migrated", 1);
        if (q_total) app_toast(T("Your apps are ready"));
        phase = 3;
    }
}

/* ---------- updates in the background ---------- */
/* the home screen shows a badge on the App Store when an installed app has an update, so the catalog is fetched now
   and then: first a little after boot (the network), then every 6 hours - only from the home screen, so a failed
   fetch never replaces the list while the App Store is open */
#define CAT_CHECK_EVERY (6 * 3600.0)
static void bg_check_tick(void) {
    static double next = -1;
    if (getenv("TVSHELL_SCRIPT") || !cfg_int("setup_done", 0)) return;
    double now = app_time();
    if (next < 0) next = now + 120;
    if (now < next || state == CAT_LOADING) return;
    Screen *top = app_top();
    if (!top || !top->name || strcmp(top->name, "home")) return;
    next = now + (state == CAT_ERROR ? 1800 : CAT_CHECK_EVERY);
    appcat_refresh();
}

/* an app that is installed (app_<id>=1) but has no package - e.g. renamed (apps.c RENAMED_APPS: the old package
   was deleted) or a package folder that went missing: downloaded again, once per fetched catalog */
static int missing_gen;
static void missing_tick(void) {
    if (state != CAT_OK || missing_gen == cat_gen || !cfg_int("apps_migrated", 0) || appcat_busy(NULL, NULL, NULL)) return;
    missing_gen = cat_gen;
    int n = 0;
    for (int i = 0; i < appcat_count(); i++) {
        const CatApp *c = appcat_at(i);
        char k[48];
        snprintf(k, sizeof k, "app_%s", c->id);
        /* a skipped app (it crashed tvshell) only when the catalog has a newer version than the one that crashed */
        if (atoi(cfg_get(k, "0")) != 0 && !app_find(c->id) && !apps_crash_skipped(c->id, c->version)) { appcat_install(c->id); n++; }
    }
    if (n) SDL_Log("appcat: %d installed apps had no package - downloading them", n);
}

void appcat_tick(void) {
    migrate_tick();
    missing_tick();
    bg_check_tick();
}
