/* NotaTV shell - helper functions: jobs, HTTP, configuration, strings */
#define _GNU_SOURCE /* pipe2 */
#include "tv.h"
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------- strenge ---------- */
char *str_dup(const char *s) { return s ? strdup(s) : NULL; }

char *str_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *out = NULL;
    if (vasprintf(&out, fmt, ap) < 0) out = NULL;
    va_end(ap);
    return out;
}

void str_trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

char *fmt_duration(double secs, char *buf, size_t n) {
    if (secs < 0) secs = 0;
    int t = (int)secs;
    if (t >= 3600) snprintf(buf, n, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(buf, n, "%d:%02d", t / 60, t % 60);
    return buf;
}

/* ---------- jobs ----------
 * Three queues with their own threads, so fast API calls (login, Plex, Seerr) never wait
 * behind images or long downloads (EPG, playlists, YouTube searches). */
typedef struct Job { JobFn work, done; void *arg; struct Job *next; } Job;
typedef struct { Job *head, *tail; SDL_Condition *cond; } JobQueue;
static SDL_Mutex *jq_lock;
static JobQueue queues[JQ_COUNT];
static Job *done_head, *done_tail;
static SDL_Thread *workers[16];
static int n_workers;
static bool jobs_quit;

static int worker_main(void *u) {
    JobQueue *q = &queues[(intptr_t)u];
    /* background work (EPG, image decoding, network) must never hurt the UI's frame rate:
       lower priority for this thread (on Linux nice applies per thread) */
    errno = 0;
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), (intptr_t)u == JQ_SLOW ? 15 : 8) != 0 && errno)
        SDL_Log("jobs: kunne ikke sænke prioritet: %s", strerror(errno));
    for (;;) {
        SDL_LockMutex(jq_lock);
        while (!q->head && !jobs_quit) SDL_WaitCondition(q->cond, jq_lock);
        if (jobs_quit) { SDL_UnlockMutex(jq_lock); return 0; }
        Job *j = q->head;
        q->head = j->next;
        if (!q->head) q->tail = NULL;
        SDL_UnlockMutex(jq_lock);

        if (j->work) j->work(j->arg);

        SDL_LockMutex(jq_lock);
        j->next = NULL;
        if (done_tail) done_tail->next = j; else done_head = j;
        done_tail = j;
        SDL_UnlockMutex(jq_lock);
    }
}

void jobs_init(int n) {
    (void)n;
    static const int threads[JQ_COUNT] = {2, 3, 2}; /* fast, images, slow (the Pi 4 has 4 cores; before 3+4+3) */
    jq_lock = SDL_CreateMutex();
    for (int q = 0; q < JQ_COUNT; q++) {
        queues[q].cond = SDL_CreateCondition();
        for (int i = 0; i < threads[q] && n_workers < 16; i++)
            workers[n_workers++] = SDL_CreateThread(worker_main, "job", (void *)(intptr_t)q);
    }
}

void jobs_shutdown(void) {
    SDL_LockMutex(jq_lock);
    jobs_quit = true;
    for (int q = 0; q < JQ_COUNT; q++) SDL_BroadcastCondition(queues[q].cond);
    SDL_UnlockMutex(jq_lock);
    for (int i = 0; i < n_workers; i++) SDL_DetachThread(workers[i]);
}

void job_submit_q(JobQueueId qi, JobFn work, JobFn done, void *arg) {
    Job *j = calloc(1, sizeof *j);
    j->work = work; j->done = done; j->arg = arg;
    JobQueue *q = &queues[qi];
    SDL_LockMutex(jq_lock);
    if (qi == JQ_IMAGE) {
        /* images: newest first - what you are looking at now must not wait behind hundreds of old
           requests (e.g. channel logos from a slow server) */
        j->next = q->head;
        q->head = j;
        if (!q->tail) q->tail = j;
    } else {
        if (q->tail) q->tail->next = j; else q->head = j;
        q->tail = j;
    }
    SDL_SignalCondition(q->cond);
    SDL_UnlockMutex(jq_lock);
}

void job_submit(JobFn work, JobFn done, void *arg) { job_submit_q(JQ_FAST, work, done, arg); }

void jobs_pump(void) {
    SDL_LockMutex(jq_lock);
    Job *list = done_head;
    done_head = done_tail = NULL;
    SDL_UnlockMutex(jq_lock);
    while (list) {
        Job *n = list->next;
        if (list->done) list->done(list->arg);
        free(list);
        list = n;
    }
}

/* ---------- HTTP ---------- */
/* Shared DNS cache, TLS sessions and connections across all requests and threads: otherwise
   a DNS lookup and a new TLS handshake are made for every single image - and a DNS server that now and
   then does not answer then costs 5 s each time (measured on the Pi against Plex). */
static CURLSH *g_share, *g_ck_share; /* g_ck_share: like g_share + shared cookies (the Seerr session) */
static SDL_AtomicInt g_ck_loaded;
static SDL_Mutex *g_share_lock[CURL_LOCK_DATA_LAST];

static void share_lock(CURL *h, curl_lock_data d, curl_lock_access a, void *u) { SDL_LockMutex(g_share_lock[d]); }
static void share_unlock(CURL *h, curl_lock_data d, void *u) { SDL_UnlockMutex(g_share_lock[d]); }

void http_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) g_share_lock[i] = SDL_CreateMutex();
    g_share = curl_share_init();
    curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    /* cookies must be shared in memory: with only a jar file, parallel calls read an old copy
       and overwrite the new login cookie when they finish (Seerr: "Signing in…" and 401) */
    g_ck_share = curl_share_init();
    curl_share_setopt(g_ck_share, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(g_ck_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(g_ck_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(g_ck_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(g_ck_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(g_ck_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
}

/* limits: http/https only (also on redirects - no file://, ftp:// etc. from a malicious server),
   replies in memory max 64 MB (big IPTV playlists), files max 1 GB */
#define HTTP_MAX_MEM (64u << 20)
#define HTTP_MAX_FILE (1024LL << 20)

static CURL *http_handle(void) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    if (g_share) {
        curl_easy_setopt(c, CURLOPT_SHARE, g_share);
        curl_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    }
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    return c;
}

static size_t write_cb(char *ptr, size_t sz, size_t nm, void *ud) {
    HttpBuf *b = ud;
    size_t n = sz * nm;
    if (b->len + n > HTTP_MAX_MEM) return 0; /* aborts the transfer */
    char *nd = realloc(b->data, b->len + n + 1);
    if (!nd) return 0;
    b->data = nd;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = 0;
    return n;
}

bool http_request(const char *method, const char *url, const char *const *headers,
                  const char *body, HttpBuf *out, long timeout_s) {
    return http_request_cookies(method, url, headers, body, out, timeout_s, NULL);
}

bool http_request_cookies(const char *method, const char *url, const char *const *headers,
                          const char *body, HttpBuf *out, long timeout_s, const char *cookie_jar) {
    memset(out, 0, sizeof *out);
    CURL *c = http_handle();
    if (!c) return false;
    struct curl_slist *hl = NULL;
    for (int i = 0; headers && headers[i]; i++) hl = curl_slist_append(hl, headers[i]);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_USERAGENT, "NotaTV/" TV_VERSION);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (method && strcmp(method, "GET") != 0) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (cookie_jar) {
        if (g_ck_share) curl_easy_setopt(c, CURLOPT_SHARE, g_ck_share);
        /* the jar file is only loaded the first time; after that the shared cookies in memory are the truth */
        bool first = !g_ck_share || SDL_CompareAndSwapAtomicInt(&g_ck_loaded, 0, 1);
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, first ? cookie_jar : "");
        curl_easy_setopt(c, CURLOPT_COOKIEJAR, cookie_jar);
    }
    if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    else if (method && strcmp(method, "POST") == 0) curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &out->status);
    if (debug_log_on()) {
        double t = 0;
        curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &t);
        char *lu = log_url(url);
        SDL_Log("http: %s %s -> %ld, %zu bytes, %.2f s%s%s", method ? method : "GET", lu, out->status, out->len, t,
                rc != CURLE_OK ? ", " : "", rc != CURLE_OK ? curl_easy_strerror(rc) : "");
        free(lu);
    }
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        { char *lu = log_url(url); SDL_Log("http: %s fejlede: %s", lu, curl_easy_strerror(rc)); free(lu); }
        return false;
    }
    return true;
}

bool http_get(const char *url, const char *const *headers, HttpBuf *out) {
    return http_request("GET", url, headers, NULL, out, 30);
}

static size_t file_cb(char *ptr, size_t sz, size_t nm, void *ud) { return fwrite(ptr, sz, nm, ud); }

bool http_download_ua(const char *url, const char *path, long timeout_s, const char *ua) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    CURL *c = http_handle();
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua ? ua : "NotaTV/" TV_VERSION);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_cb);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)HTTP_MAX_FILE);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (debug_log_on()) {
        double t = 0;
        curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &t);
        char *lu = log_url(url);
        SDL_Log("http: download %s -> %ld, %.2f s", lu, status, t);
        free(lu);
    }
    curl_easy_cleanup(c);
    fclose(f);
    bool ok = rc == CURLE_OK && (status == 200 || status == 0);
    if (ok) rename(tmp, path);
    else {
        { char *lu = log_url(url); SDL_Log("download %s fejlede (%s, %ld)", lu, curl_easy_strerror(rc), status); free(lu); }
        unlink(tmp);
    }
    return ok;
}

bool http_download(const char *url, const char *path, long timeout_s) {
    return http_download_ua(url, path, timeout_s, NULL);
}

void http_free(HttpBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
}

/* percent encoding like curl_easy_escape (everything except A-Z a-z 0-9 - . _ ~), but without creating a
 * CURL handle per call - it is used for every visible poster in every frame (Plex) */
char *url_encode(const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    if (!s) s = "";
    char *r = malloc(strlen(s) * 3 + 1), *w = r;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '.' || *p == '_' || *p == '~') {
            *w++ = *p;
        } else {
            *w++ = '%';
            *w++ = hex[*p >> 4];
            *w++ = hex[*p & 15];
        }
    }
    *w = 0;
    return r;
}

/* ---------- configuration (key=value) ---------- */
typedef struct { char *k, *v; } Kv;
static Kv *cfg;
static int n_cfg;
/* background threads read the configuration: protect it with a lock, and never free old values
   (a thread may still hold a pointer to them - the number of changes is small) */
static SDL_Mutex *cfg_lock;
static char cfg_path[512];
static char g_data_dir[256];

const char *data_dir(void) { return g_data_dir; }

/* ---------- troubleshooting log ---------- */
static SDL_AtomicInt g_debug_log; /* read from worker threads (http) */
bool debug_log_on(void) { return SDL_GetAtomicInt(&g_debug_log) != 0; }
static char *log_path(const char *name, bool keep) {
    if (!keep) return str_printf("/tmp/%s", name);
    char *d = str_printf("%s/logs", data_dir());
    mkdir(d, 0700);
    char *p = str_printf("%s/%s", d, name);
    free(d);
    return p;
}
const char *mpv_log_path(void) {
    static char p[512];
    char *l = log_path("mpv.log", debug_log_on());
    snprintf(p, sizeof p, "%s", l);
    free(l);
    return p;
}
void debug_log_apply(void) {
    bool on = cfg_int("debug_log", 0) != 0;
    SDL_SetAtomicInt(&g_debug_log, on);
    /* the user interface's own log: on the box (stderr goes to /tmp/tvshell.log or the kept log) it moves along */
    char cur[512] = "";
    ssize_t n = readlink("/proc/self/fd/2", cur, sizeof cur - 1);
    if (n <= 0) return;
    cur[n] = 0;
    char *keep = log_path("tvshell.log", true), *tmp = log_path("tvshell.log", false);
    const char *want = on ? keep : tmp;
    if ((!strcmp(cur, keep) || !strcmp(cur, tmp)) && strcmp(cur, want)) {
        if (on) { /* the previous kept log becomes .1 (it may tell about a crash before a restart) */
            char *old = str_printf("%s.1", keep);
            rename(keep, old);
            free(old);
        }
        fflush(stderr);
        fflush(stdout);
        if (freopen(want, "a", stderr)) dup2(fileno(stderr), 1);
        SDL_Log("troubleshooting log %s (NotaTV %s)", on ? "on" : "off", TV_VERSION);
    }
    free(keep);
    free(tmp);
}

static void mkdirs(const char *p) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", p);
    for (char *s = tmp + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(tmp, 0755); *s = '/'; }
    mkdir(tmp, 0755);
}

void cfg_load(void) {
    if (!cfg_lock) cfg_lock = SDL_CreateMutex();
    const char *d = getenv("TVSHELL_DATA");
    snprintf(g_data_dir, sizeof g_data_dir, "%s", d ? d : "/data/notatv");
    mkdirs(g_data_dir);
    snprintf(cfg_path, sizeof cfg_path, "%s/notatv.conf", g_data_dir);
    for (int i = 0; i < n_cfg; i++) { free(cfg[i].k); free(cfg[i].v); }
    free(cfg);
    cfg = NULL;
    n_cfg = 0;
    FILE *f = fopen(cfg_path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *v = eq + 1;
        size_t n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r')) v[--n] = 0;
        str_trim(line);
        cfg = realloc(cfg, sizeof(Kv) * (n_cfg + 1));
        cfg[n_cfg].k = strdup(line);
        cfg[n_cfg].v = strdup(v);
        n_cfg++;
    }
    fclose(f);
}

const char *cfg_get(const char *key, const char *def) {
    const char *r = def;
    SDL_LockMutex(cfg_lock);
    for (int i = 0; i < n_cfg; i++)
        if (strcmp(cfg[i].k, key) == 0) { r = cfg[i].v; break; }
    SDL_UnlockMutex(cfg_lock);
    return r;
}

int cfg_int(const char *key, int def) {
    const char *v = cfg_get(key, NULL);
    return v && *v ? atoi(v) : def;
}

static void cfg_save(void) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", cfg_path);
    FILE *f = fopen(tmp, "w");
    if (!f) { SDL_Log("kan ikke gemme %s: %s", cfg_path, strerror(errno)); return; }
    for (int i = 0; i < n_cfg; i++) fprintf(f, "%s=%s\n", cfg[i].k, cfg[i].v);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, cfg_path);
}

/* the configuration is stored as "key=value" per line: a newline in a value (e.g. from the phone's
 * web page, %0A) could otherwise add a new setting like ssh_enabled=1 */
static void cfg_set_clean(const char *key, const char *val);
static char *cfg_clean(const char *s) {
    char *o = strdup(s);
    for (unsigned char *p = (unsigned char *)o; *p; p++)
        if (*p < 0x20 || *p == 0x7f) *p = ' ';
    return o;
}

void cfg_set(const char *key_in, const char *val_in) {
    if (!val_in) val_in = "";
    char *key = cfg_clean(key_in), *val = cfg_clean(val_in);
    for (char *p = key; *p; p++) if (*p == '=') *p = '_';
    cfg_set_clean(key, val);
    free(key);
    free(val);
}

static void cfg_set_clean(const char *key, const char *val) {
    SDL_LockMutex(cfg_lock);
    for (int i = 0; i < n_cfg; i++)
        if (strcmp(cfg[i].k, key) == 0) {
            if (strcmp(cfg[i].v, val) != 0) {
                cfg[i].v = strdup(val); /* the old value is leaked on purpose (see cfg_lock) */
                cfg_save();
            }
            SDL_UnlockMutex(cfg_lock);
            return;
        }
    /* a new array instead of realloc, so readers that are iterating do not hit freed memory */
    Kv *n = malloc(sizeof(Kv) * (n_cfg + 1));
    if (n_cfg) memcpy(n, cfg, sizeof(Kv) * n_cfg);
    n[n_cfg].k = strdup(key);
    n[n_cfg].v = strdup(val);
    cfg = n;
    n_cfg++;
    cfg_save();
    SDL_UnlockMutex(cfg_lock);
}

/* remove a setting (the default applies again) - e.g. an app's settings when it is uninstalled */
void cfg_unset(const char *key) {
    SDL_LockMutex(cfg_lock);
    for (int i = 0; i < n_cfg; i++)
        if (strcmp(cfg[i].k, key) == 0) {
            /* a new array (the old one and the strings are leaked on purpose, like cfg_set: readers may hold them) */
            Kv *n = malloc(sizeof(Kv) * (n_cfg > 1 ? n_cfg - 1 : 1));
            int k = 0;
            for (int j = 0; j < n_cfg; j++) if (j != i) n[k++] = cfg[j];
            cfg = n;
            n_cfg = k;
            cfg_save();
            break;
        }
    SDL_UnlockMutex(cfg_lock);
}

void cfg_set_int(const char *key, int v) {
    char b[32];
    snprintf(b, sizeof b, "%d", v);
    cfg_set(key, b);
}

/* ---------- systemkald ---------- */
const char *helper_path(void) {
    const char *e = getenv("TVSHELL_HELPERS");
    return e ? e : "/usr/libexec/notatv";
}

char *run_capture(const char *cmd) {
    FILE *p = popen(cmd, "r");
    if (!p) return str_dup("");
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, p)) > 0) {
        len += n;
        if (cap - len < 1024) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[len] = 0;
    pclose(p);
    return buf;
}

/* URL for log files without secrets: values of token/password parameters and Xtream paths
 * (/live|movie|series/<user>/<pass>/...) are replaced by "***". malloc'ed. */
char *log_url(const char *url) {
    if (!url) return str_dup("(null)");
    static const char *keys[] = {"x-plex-token", "token", "api_key", "apikey", "password", "pass", "username", "user", "auth", NULL};
    size_t n = strlen(url);
    char *o = malloc(2 * n + 64), *w = o; /* each masking can make the text at most 1.5x longer */
    const char *p = url, *q = strchr(url, '?');
    /* path: Xtream credentials */
    const char *end = q ? q : url + n;
    while (p < end) {
        const char *seg = NULL;
        if (!strncmp(p, "/live/", 6)) seg = p + 6;
        else if (!strncmp(p, "/movie/", 7)) seg = p + 7;
        else if (!strncmp(p, "/series/", 8)) seg = p + 8;
        if (seg) {
            const char *s1 = memchr(seg, '/', (size_t)(end - seg));
            const char *s2 = s1 ? memchr(s1 + 1, '/', (size_t)(end - s1 - 1)) : NULL;
            if (s2) {
                memcpy(w, p, (size_t)(seg - p)); w += seg - p;
                memcpy(w, "***/***", 7); w += 7;
                p = s2;
                continue;
            }
        }
        *w++ = *p++;
    }
    /* query */
    while (*p) {
        *w++ = *p++; /* '?' or '&' */
        const char *eq = strchr(p, '='), *amp = strchr(p, '&');
        if (!amp) amp = p + strlen(p);
        bool secret = false;
        if (eq && eq < amp)
            for (int i = 0; keys[i]; i++)
                if ((size_t)(eq - p) == strlen(keys[i]) && !strncasecmp(p, keys[i], (size_t)(eq - p))) secret = true;
        if (secret) {
            memcpy(w, p, (size_t)(eq - p + 1)); w += eq - p + 1;
            memcpy(w, "***", 3); w += 3;
        } else {
            memcpy(w, p, (size_t)(amp - p)); w += amp - p;
        }
        p = amp;
    }
    *w = 0;
    return o;
}

/* like run_capture, but without a shell: argv[0] is run directly (execvp), so arguments from the network
 * (URLs, video ids) are never interpreted as shell code. stderr is thrown away. */
char *run_capture_argv(const char *const *argv) {
    int fd[2];
    if (pipe2(fd, O_CLOEXEC) < 0) return str_dup("");
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return str_dup(""); }
    if (pid == 0) {
        dup2(fd[1], STDOUT_FILENO);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); dup2(dn, STDERR_FILENO); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(fd[1]);
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    ssize_t n;
    while ((n = read(fd[0], buf + len, cap - len - 1)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; break; }
        len += n;
        if (cap - len < 1024) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[len] = 0;
    close(fd[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    return buf;
}

int run_cmd(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *cmd = NULL;
    if (vasprintf(&cmd, fmt, ap) < 0) { va_end(ap); return -1; }
    va_end(ap);
    int rc = system(cmd);
    free(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* Language: the config key language (en/da/de/es/fr). Boxes from before the language choice existed ran in Danish:
 * if the setup guide has run but no language is chosen, Danish is kept. Otherwise English is the default. */
void lang_init(void) {
    const char *l = cfg_get("language", "");
    if (!*l && cfg_int("setup_done", 0)) { cfg_set("language", "da"); l = "da"; }
    if (!i18n_load(asset_dir(), *l ? l : "en")) SDL_Log("sprog: kunne ikke indlæse %s", l);
    setenv("NOTATV_LANG", i18n_lang(), 1); /* tvmoon and mpv's OSD script read the same language file */
}

/* Message from a helper script (English): "Text" or "Text with %s\037arg1\037arg2" (up to 3 %s).
 * Translated and filled in; the last line is used if there are several. Ring buffer: valid for a while. */
const char *tr_msg(const char *msg) {
    static char ring[4][512];
    static int ri;
    if (!msg) return "";
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", msg);
    size_t n = strlen(tmp);
    while (n && (tmp[n - 1] == '\n' || tmp[n - 1] == ' ')) tmp[--n] = 0;
    char *line = strrchr(tmp, '\n');
    line = line ? line + 1 : tmp;
    const char *a[3] = {"", "", ""};
    char *sep = strchr(line, '\037');
    bool args = sep != NULL;
    for (int i = 0; i < 3 && sep; i++) {
        *sep = 0;
        a[i] = sep + 1;
        sep = strchr(sep + 1, '\037');
    }
    if (sep) *sep = 0;
    char *out = ring[ri++ & 3];
    const char *fmt = T(line);
    bool ok = args; /* only %s and %% in the format */
    for (const char *p = fmt; ok && (p = strchr(p, '%')); p += 2)
        if (p[1] != 's' && p[1] != '%') ok = false;
    if (ok) snprintf(out, 512, fmt, a[0], a[1], a[2]);
    else if (args || fmt != line) snprintf(out, 512, "%s", args ? line : fmt);
    else { /* not found: translate the parts "a · b · text: value" one by one (only the text before the colon) */
        out[0] = 0;
        for (char *seg = line, *nx; seg; seg = nx) {
            nx = strstr(seg, "·"); /* the whole UTF-8 character (strtok would split on single bytes) */
            if (nx) { *nx = 0; nx += strlen("·"); }
            while (*seg == ' ') seg++;
            size_t sl = strlen(seg);
            while (sl && seg[sl - 1] == ' ') seg[--sl] = 0;
            size_t ol = strlen(out);
            if (ol) { snprintf(out + ol, 512 - ol, " · "); ol = strlen(out); }
            const char *t = T(seg);
            char *colon = t == seg ? strstr(seg, ": ") : NULL;
            if (colon) {
                *colon = 0;
                snprintf(out + ol, 512 - ol, "%s: %s", T(seg), colon + 2); /* the value (e.g. a network name) is not translated */
            } else snprintf(out + ol, 512 - ol, "%s", t);
        }
    }
    return out;
}

/* the default subtitle/audio language follows the UI language (until the user chooses):
 * Danish = Danish subtitles and Danish audio first; English = English audio and no subtitles */
const char *lang_media_default(const char *key) {
    bool da = !strcmp(i18n_lang(), "da");
    if (!strcmp(key, "sub_lang")) return da ? "da,dan,dk,en,eng" : "no-subs";
    if (!strcmp(key, "audio_lang")) return da ? "da,dan,en,eng" : "en,eng";
    return NULL;
}

/* ---------- signing out: nothing of the account is left behind ---------- */
/* small files are overwritten with zeros first, then deleted */
void wipe_file(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= (1 << 20)) {
        FILE *f = fopen(path, "r+");
        if (f) {
            static const char z[4096];
            for (off_t n = st.st_size; n > 0; n -= (off_t)sizeof z) fwrite(z, 1, n > (off_t)sizeof z ? sizeof z : (size_t)n, f);
            fflush(f);
            fsync(fileno(f));
            fclose(f);
        }
    }
    unlink(path);
}
/* everything in a folder (and below), the folder itself stays */
void wipe_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) { wipe_dir(p); rmdir(p); }
        else wipe_file(p);
    }
    closedir(d);
}
/* leftovers in /tmp that can hold addresses, tokens or pictures from the account (mpv log, image cache, now playing) */
void wipe_session_traces(void) {
    wipe_dir("/tmp/tvimg");
    /* mpv-out.log holds the stream URLs with tokens (Plex/Jellyfin), the yt-dlp log searches, and the subtitle
       files and channel logos show what was watched */
    static const char *const files[] = {"/tmp/mpv.log", "/tmp/mpv-out.log", "/tmp/yt-dlp-search.log", "/tmp/notatv-nowplaying.json",
                                        "/tmp/notatv-subs.json", "/tmp/notatv-poster.bgra", "/tmp/notatv-poster-hdr.bgra", NULL};
    for (int i = 0; files[i]; i++) wipe_file(files[i]);
    static const char *const pats[] = {"/tmp/notatv-sub-*.srt", "/tmp/notatv-logo-*.bgra", NULL};
    for (int i = 0; pats[i]; i++) {
        glob_t g;
        if (glob(pats[i], 0, NULL, &g) != 0) continue;
        for (size_t k = 0; k < g.gl_pathc; k++) wipe_file(g.gl_pathv[k]);
        globfree(&g);
    }
    sync();
}
