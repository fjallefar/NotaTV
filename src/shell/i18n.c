/* NotaTV - translations. The source text (msgid) is English; other languages are gettext .po files
 * in <assets>/lang/<code>.po. Self-contained file (libc only), so src/moon can use an identical copy.
 * T(text) looks up; N_(text) marks text in static tables, which is translated when shown. */
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *id, *str; } Entry;
static Entry *tab;
static size_t cap, used;
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
    if ((used + 1) * 2 > cap) { /* double and move */
        size_t ncap = cap ? cap * 2 : 1024;
        Entry *n = calloc(ncap, sizeof *n);
        if (!n) { free(id); free(str); return; }
        for (size_t i = 0; i < cap; i++)
            if (tab[i].id) {
                size_t j = hash(tab[i].id) & (ncap - 1);
                while (n[j].id) j = (j + 1) & (ncap - 1);
                n[j] = tab[i];
            }
        free(tab); tab = n; cap = ncap;
    }
    size_t j = hash(id) & (cap - 1);
    while (tab[j].id) {
        if (!strcmp(tab[j].id, id)) { free(tab[j].str); tab[j].str = str; free(id); return; }
        j = (j + 1) & (cap - 1);
    }
    tab[j].id = id; tab[j].str = str; used++;
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
    return load_po(path);
}

const char *T(const char *s) {
    if (!s || !used) return s;
    size_t j = hash(s) & (cap - 1);
    while (tab[j].id) {
        if (!strcmp(tab[j].id, s)) return tab[j].str;
        j = (j + 1) & (cap - 1);
    }
    return s;
}

const char *i18n_lang(void) { return cur; }
