/* NotaTV - translations (see i18n.c). Identical copy in src/moon. */
#pragma once
#include <stdbool.h>

typedef struct { const char *code, *name; } LangInfo;
extern const LangInfo i18n_langs[]; /* {"en","English"}, ... terminated by {NULL,NULL} */

bool i18n_load(const char *assets_dir, const char *lang); /* lang = "en" or "da" */
const char *T(const char *s);  /* translated text (or s itself) */
const char *i18n_lang(void);   /* active language code */
#define N_(s) (s)              /* marks text that is translated later with T() */
