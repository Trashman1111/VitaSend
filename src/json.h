/*
 * json.h - tiny JSON reader + writer (no dependencies).
 * Enough for every LocalSend message, including the nested ones used by later phases.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_JSON_H
#define LS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------ reader */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } json_type;

typedef struct json_val json_val;
struct json_val {
    json_type type;
    bool      b;
    double    num;
    char     *str;                 /* J_STR (NUL terminated, UTF-8) */
    size_t    n;                   /* J_ARR / J_OBJ element count    */
    json_val **items;              /* J_ARR values / J_OBJ values    */
    char    **keys;                /* J_OBJ keys                     */
};

/* Parses text[0..len). Returns NULL on error (message copied to err if given). Free with json_free. */
json_val *json_parse(const char *text, size_t len, char *err, size_t errcap);
void      json_free(json_val *v);

/* Lookup helpers - all tolerate NULL, so  json_str(json_get(o,"x"), "default")  is always safe. */
const json_val *json_get(const json_val *obj, const char *key);
const char     *json_str(const json_val *v, const char *def);
double          json_num(const json_val *v, double def);
bool            json_bool(const json_val *v, bool def);

/* ------------------------------------------------------------ writer */
#define JW_MAX_DEPTH 16
typedef struct {
    char  *buf;                    /* NUL terminated, owned (free with jw_free) */
    size_t len, cap;
    int    depth;
    bool   first[JW_MAX_DEPTH];
    bool   after_key;
    bool   oom;
} jw;

void jw_init(jw *w);
void jw_free(jw *w);
void jw_obj(jw *w);                        /* {  */
void jw_end_obj(jw *w);                    /* }  */
void jw_arr(jw *w);                        /* [  */
void jw_end_arr(jw *w);                    /* ]  */
void jw_key(jw *w, const char *key);       /* "key":  - next call writes the value */
void jw_str(jw *w, const char *s);         /* NULL -> null */
void jw_int(jw *w, int64_t v);
void jw_bool(jw *w, bool v);
void jw_null(jw *w);
/* shorthands: key + value */
void jw_kstr(jw *w, const char *key, const char *s);
void jw_kint(jw *w, const char *key, int64_t v);
void jw_kbool(jw *w, const char *key, bool v);

#endif
