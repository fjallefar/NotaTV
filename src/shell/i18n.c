/* NotaTV - translations. The source text (msgid) is English; other languages are gettext .po files
 * in <assets>/lang/<code>.po. Self-contained file (libc only), so src/moon can use an identical copy.
 * T(text) looks up; N_(text) marks text in static tables, which is translated when shown. */
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The table is read by T() from any thread while an app package may add its texts on the main thread. So:
   a full table is replaced by a bigger copy (the old one is left, never freed), an entry's text is in place
   before its id becomes visible, and a text that is replaced is not freed (pointers to it may be in use). */
typedef struct { char *id, *str; } Entry;
typedef struct { Entry *e; size_t cap; } Table;
static Table *tbl;
static size_t used;
static bool loading_extra; /* an app's texts never replace the system's own */
static char cur[8] = "en";

const LangInfo i18n_langs[] = {
    {"en", "English"}, {"da", "Dansk"}, {NULL, NULL},
};

static size_t hash(const char *s) {
    size_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static void put(char *id, char *str) {
    if (!*id || !*str) { free(id); free(str); return; }
    Table *t = tbl;
    if (!t || (used + 1) * 2 > t->cap) { /* a bigger copy; readers keep using the old one until it is published */
        size_t ncap = t ? t->cap * 2 : 1024;
        Table *n = calloc(1, sizeof *n);
        Entry *e = n ? calloc(ncap, sizeof *e) : NULL;
        if (!e) { free(n); free(id); free(str); return; }
        for (size_t i = 0; t && i < t->cap; i++)
            if (t->e[i].id) {
                size_t j = hash(t->e[i].id) & (ncap - 1);
                while (e[j].id) j = (j + 1) & (ncap - 1);
                e[j] = t->e[i];
            }
        n->e = e;
        n->cap = ncap;
        __atomic_store_n(&tbl, n, __ATOMIC_RELEASE);
        t = n;
    }
    size_t j = hash(id) & (t->cap - 1);
    while (t->e[j].id) {
        if (!strcmp(t->e[j].id, id)) {
            if (loading_extra) free(str); /* the system's text stays */
            else __atomic_store_n(&t->e[j].str, str, __ATOMIC_RELEASE); /* the old text is not freed */
            free(id);
            return;
        }
        j = (j + 1) & (t->cap - 1);
    }
    t->e[j].str = str;
    __atomic_store_n(&t->e[j].id, id, __ATOMIC_RELEASE);
    used++;
}

/* "..." with C escapes -> appended to buf */
static void unquote(const char *p, char **buf, size_t *len) {
    p = strchr(p, '"');
    if (!p) return;
    p++;
    size_t n = strlen(p);
    char *b = realloc(*buf, *len + n + 1);
    if (!b) return;
    *buf = b;
    for (; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
        }
        b[(*len)++] = c;
    }
    b[*len] = 0;
}

static bool load_po(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[4096];
    char *id = NULL, *str = NULL;
    size_t idl = 0, strl = 0;
    int state = 0; /* 1 = i msgid, 2 = i msgstr */
    bool fuzzy = false;
    for (;;) {
        bool eof = !fgets(line, sizeof line, f);
        const char *p = line;
        while (!eof && (*p == ' ' || *p == '\t')) p++;
        bool cont = !eof && *p == '"';
        if (!cont && state == 2) { /* the entry has ended */
            if (!fuzzy && id && str) put(id, str); else { free(id); free(str); }
            id = str = NULL; idl = strl = 0; state = 0; fuzzy = false;
        }
        if (eof) break;
        if (cont) { if (state == 1) unquote(p, &id, &idl); else if (state == 2) unquote(p, &str, &strl); }
        else if (!strncmp(p, "#,", 2) && strstr(p, "fuzzy")) fuzzy = true;
        else if (!strncmp(p, "msgid ", 6)) { free(id); id = calloc(1, 1); idl = 0; unquote(p + 5, &id, &idl); state = 1; }
        else if (!strncmp(p, "msgstr ", 7)) { free(str); str = calloc(1, 1); strl = 0; unquote(p + 6, &str, &strl); state = 2; }
    }
    free(id); free(str);
    fclose(f);
    return true;
}

bool i18n_load(const char *dir, const char *lang) {
    if (!lang || !*lang) lang = "en";
    bool known = false;
    for (const LangInfo *l = i18n_langs; l->code; l++) if (!strcmp(l->code, lang)) known = true;
    if (!known) lang = "en";
    snprintf(cur, sizeof cur, "%s", lang);
    if (!strcmp(lang, "en")) return true; /* the source text is English */
    char path[512];
    snprintf(path, sizeof path, "%s/lang/%s.po", dir, lang);
    return load_po(path);
}

/* extra translations (an app package): merged into the same table */
bool i18n_load_extra(const char *dir) {
    if (!strcmp(cur, "en") || !*cur) return true;
    char path[512];
    snprintf(path, sizeof path, "%s/lang/%s.po", dir, cur);
    loading_extra = true;
    bool ok = load_po(path);
    loading_extra = false;
    return ok;
}

const char *T(const char *s) {
    Table *t = __atomic_load_n(&tbl, __ATOMIC_ACQUIRE);
    if (!s || !t) return s;
    size_t j = hash(s) & (t->cap - 1);
    const char *id;
    while ((id = __atomic_load_n(&t->e[j].id, __ATOMIC_ACQUIRE))) {
        if (!strcmp(id, s)) return __atomic_load_n(&t->e[j].str, __ATOMIC_ACQUIRE);
        j = (j + 1) & (t->cap - 1);
    }
    return s;
}

const char *i18n_lang(void) { return cur; }
