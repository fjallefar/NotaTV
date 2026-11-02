/* NotaTV shell - subtitles from OpenSubtitles (REST API v1) */
#include "tv.h"
#include "vendor/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OS_API "https://api.opensubtitles.com/api/v1"
#define OS_UA "NotaTV v" TV_VERSION

static int found_ids[30];
static int n_found;
static char token[512];

static const char *const *os_headers(char buf[4][600], const char **out, bool auth) {
    int n = 0;
    snprintf(buf[n], 600, "Api-Key: %s", cfg_get("os_apikey", ""));
    out[n] = buf[n]; n++;
    snprintf(buf[n], 600, "User-Agent: %s", OS_UA);
    out[n] = buf[n]; n++;
    snprintf(buf[n], 600, "Content-Type: application/json");
    out[n] = buf[n]; n++;
    if (auth && token[0]) { snprintf(buf[n], 600, "Authorization: Bearer %s", token); out[n] = buf[n]; n++; }
    out[n] = NULL;
    return out;
}

static const char *lang_name(const char *c) {
    static const char *map[][2] = {{"da", N_("Danish")}, {"en", N_("English")}, {"sv", N_("Swedish")}, {"no", N_("Norwegian")}, {"de", N_("German")},
                                   {"fr", N_("French")}, {"es", N_("Spanish")}, {"ar", N_("Arabic")}, {"tr", N_("Turkish")}, {"pl", N_("Polish")}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcmp(map[i][0], c)) return T(map[i][1]);
    return c;
}

/* language list from the subtitle setting (e.g. "da,dan,dk,en,eng" -> "da,en") */
static void os_langs(char *out, size_t n) {
    out[0] = 0;
    char tmp[128];
    snprintf(tmp, sizeof tmp, "%s", cfg_get("sub_lang", lang_media_default("sub_lang")));
    for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
        if (strlen(t) != 2 || !strcmp(t, "dk")) continue;
        if (strstr(out, t)) continue;
        if (out[0]) strncat(out, ",", n - strlen(out) - 1);
        strncat(out, t, n - strlen(out) - 1);
    }
    if (!out[0]) snprintf(out, n, "%s", strcmp(i18n_lang(), "da") ? "en" : "da,en");
}

int subs_search(int tmdb, const char *query, int season, int episode, char ***labels) {
    *labels = NULL;
    n_found = 0;
    if (!*cfg_get("os_apikey", "")) {
        *labels = calloc(1, sizeof(char *));
        (*labels)[0] = str_dup(T("Requires an OpenSubtitles key: Settings → Picture and sound"));
        return 1;
    }
    char langs[64];
    os_langs(langs, sizeof langs);
    char *url;
    if (tmdb > 0 && season <= 0) url = str_printf(OS_API "/subtitles?tmdb_id=%d&languages=%s&order_by=download_count", tmdb, langs);
    else {
        char *q = url_encode(query ? query : "");
        if (season > 0) url = str_printf(OS_API "/subtitles?query=%s&season_number=%d&episode_number=%d&languages=%s&order_by=download_count", q, season, episode, langs);
        else url = str_printf(OS_API "/subtitles?query=%s&languages=%s&order_by=download_count", q, langs);
        free(q);
    }
    char hb[4][600];
    const char *hp[5];
    HttpBuf b;
    bool ok = http_request("GET", url, os_headers(hb, hp, false), NULL, &b, 15);
    free(url);
    cJSON *root = ok && b.data ? cJSON_Parse(b.data) : NULL;
    http_free(&b);
    cJSON *d;
    char **out = calloc(31, sizeof(char *));
    int n = 0;
    cJSON_ArrayForEach(d, cJSON_GetObjectItem(root, "data")) {
        if (n >= 30) break;
        cJSON *at = cJSON_GetObjectItem(d, "attributes");
        cJSON *files = cJSON_GetObjectItem(at, "files"), *f0 = cJSON_GetArrayItem(files, 0);
        cJSON *fid = cJSON_GetObjectItem(f0, "file_id");
        if (!cJSON_IsNumber(fid)) continue;
        cJSON *lang = cJSON_GetObjectItem(at, "language"), *rel = cJSON_GetObjectItem(at, "release");
        cJSON *dl = cJSON_GetObjectItem(at, "download_count"), *hi = cJSON_GetObjectItem(at, "hearing_impaired");
        found_ids[n] = fid->valueint;
        out[n] = str_printf(T("%s · %s%s · %d downloads"), cJSON_IsString(lang) ? lang_name(lang->valuestring) : "?",
                            cJSON_IsString(rel) ? rel->valuestring : "", cJSON_IsTrue(hi) ? " (HI)" : "",
                            cJSON_IsNumber(dl) ? dl->valueint : 0);
        n++;
    }
    cJSON_Delete(root);
    n_found = n;
    *labels = out;
    if (n == 0) { free(out); *labels = NULL; }
    return n;
}

static bool os_login(void) {
    if (token[0]) return true;
    const char *u = cfg_get("os_user", ""), *p = cfg_get("os_pass", "");
    if (!*u) return false;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "username", u);
    cJSON_AddStringToObject(o, "password", p);
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char hb[4][600];
    const char *hp[5];
    HttpBuf b;
    bool ok = http_request("POST", OS_API "/login", os_headers(hb, hp, false), body, &b, 15);
    free(body);
    cJSON *root = ok && b.data ? cJSON_Parse(b.data) : NULL;
    http_free(&b);
    cJSON *t = cJSON_GetObjectItem(root, "token");
    if (cJSON_IsString(t)) snprintf(token, sizeof token, "%s", t->valuestring);
    cJSON_Delete(root);
    return token[0] != 0;
}

char *subs_fetch(int idx) {
    if (idx < 0 || idx >= n_found) return NULL;
    os_login(); /* downloading requires an OpenSubtitles login (otherwise it is tried without) */
    char body[64];
    snprintf(body, sizeof body, "{\"file_id\":%d}", found_ids[idx]);
    char hb[4][600];
    const char *hp[5];
    HttpBuf b;
    bool ok = http_request("POST", OS_API "/download", os_headers(hb, hp, true), body, &b, 15);
    cJSON *root = ok && b.data ? cJSON_Parse(b.data) : NULL;
    http_free(&b);
    cJSON *link = cJSON_GetObjectItem(root, "link");
    char *path = NULL;
    if (cJSON_IsString(link)) {
        path = str_printf("/tmp/notatv-sub-%d.srt", found_ids[idx]);
        if (!http_download(link->valuestring, path, 30)) { free(path); path = NULL; }
    }
    cJSON_Delete(root);
    return path;
}

/* sign out of OpenSubtitles: the key, the login and the session token */
void subs_forget(void) {
    cfg_set("os_apikey", "");
    cfg_set("os_user", "");
    cfg_set("os_pass", "");
    memset(token, 0, sizeof token);
    wipe_session_traces(); /* also the downloaded subtitle files */
}
