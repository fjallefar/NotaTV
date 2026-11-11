/* Shared cJSON helpers for the apps (audit 11/11 B9). Header-only (static inline): an app using them needs no new
 * symbol from tvshell. Only the variants that were the same in every app are here; apps with other rules keep their
 * own (Plex jstr also turns numbers into text, Peezy/App Store borrow the string instead of copying it). */
#ifndef NOTA_JSON_UTIL_H
#define NOTA_JSON_UTIL_H
#include "tv.h"
#include "vendor/cJSON.h"

/* o[k] as a number, 0 if missing or not a number (o may be NULL) */
static inline double json_num(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}
/* o[k] copied (free it), NULL if missing, not a string or empty (o may be NULL) */
static inline char *json_dup(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) && v->valuestring && v->valuestring[0] ? str_dup(v->valuestring) : NULL;
}
#endif
