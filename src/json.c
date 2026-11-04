/*
 * json.c - A small JSON parser and writer (no dependencies). The parser builds a tree of json_val; the writer (jw) appends to a buffer.
 *
 * Author: trashman (VitaSend)
 */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =================================================================== reader */

#define MAX_DEPTH 24

typedef struct {
    const char *p, *end, *start;
    int         depth;
    char       *err;
    size_t      errcap;
    bool        failed;
} parser;

static void fail(parser *ps, const char *msg)
{
    if (!ps->failed && ps->err && ps->errcap)
        snprintf(ps->err, ps->errcap, "%s (offset %d)", msg, (int)(ps->p - ps->start));
    ps->failed = true;
}

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static json_val *node(json_type t)
{
    json_val *v = calloc(1, sizeof *v);
    if (v) v->type = t;
    return v;
}

void json_free(json_val *v)
{
    if (!v) return;
    if (v->type == J_STR) free(v->str);
    if (v->type == J_ARR || v->type == J_OBJ) {
        for (size_t i = 0; i < v->n; i++) {
            json_free(v->items[i]);
            if (v->keys) free(v->keys[i]);
        }
        free(v->items);
        free(v->keys);
    }
    free(v);
}

static int hex4(const char *p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static size_t put_utf8(char *out, unsigned cp)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 63));
        out[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63));
    out[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* ps->p is on the opening quote. Returns malloc'd string, leaves ps->p after the closing quote. */
static char *parse_string(parser *ps)
{
    const char *p = ps->p + 1;
    const char *q = p;
    while (q < ps->end && *q != '"') {
        if (*q == '\\') {
            q++;
            if (q >= ps->end) break;
        }
        q++;
    }
    if (q >= ps->end) {
        fail(ps, "unterminated string");
        return NULL;
    }
    char *out = malloc((size_t)(q - p) + 1);   /* decoded text is never longer than the escaped text */
    if (!out) {
        fail(ps, "out of memory");
        return NULL;
    }
    size_t n = 0;
    while (p < q) {
        unsigned char c = (unsigned char)*p++;
        if (c != '\\') {
            out[n++] = (char)c;
            continue;
        }
        char e = *p++;
        switch (e) {
        case '"':  out[n++] = '"';  break;
        case '\\': out[n++] = '\\'; break;
        case '/':  out[n++] = '/';  break;
        case 'b':  out[n++] = '\b'; break;
        case 'f':  out[n++] = '\f'; break;
        case 'n':  out[n++] = '\n'; break;
        case 'r':  out[n++] = '\r'; break;
        case 't':  out[n++] = '\t'; break;
        case 'u': {
            int cp = (q - p >= 4) ? hex4(p) : -1;
            if (cp < 0) {
                ps->p = p;
                fail(ps, "bad \\u escape");
                free(out);
                return NULL;
            }
            p += 4;
            unsigned u = (unsigned)cp;
            if (u >= 0xD800 && u < 0xDC00) {                 /* high surrogate: need a low one */
                int lo = (q - p >= 6 && p[0] == '\\' && p[1] == 'u') ? hex4(p + 2) : -1;
                if (lo >= 0xDC00 && lo < 0xE000) {
                    u = 0x10000 + ((u - 0xD800) << 10) + ((unsigned)lo - 0xDC00);
                    p += 6;
                } else {
                    u = 0xFFFD;
                }
            } else if (u >= 0xDC00 && u < 0xE000) {
                u = 0xFFFD;
            }
            if (u == 0) u = 0xFFFD;                          /* never put a NUL inside a C string */
            n += put_utf8(out + n, u);
            break;
        }
        default:
            ps->p = p;
            fail(ps, "bad escape");
            free(out);
            return NULL;
        }
    }
    out[n] = 0;
    ps->p = q + 1;
    return out;
}

static json_val *parse_value(parser *ps);

/* Capacity is 4, then doubles whenever the count reaches a power of two (4, 8, 16, ...). */
static bool grow(json_val *v, bool is_obj)
{
    bool need = (v->n == 0) || (v->n >= 4 && (v->n & (v->n - 1)) == 0);
    if (!need) return true;
    size_t cap = v->n ? v->n * 2 : 4;
    json_val **ni = realloc(v->items, cap * sizeof *ni);
    if (!ni) return false;
    v->items = ni;
    if (is_obj) {
        char **nk = realloc(v->keys, cap * sizeof *nk);
        if (!nk) return false;
        v->keys = nk;
    }
    return true;
}

static json_val *parse_array(parser *ps)
{
    json_val *v = node(J_ARR);
    if (!v) { fail(ps, "out of memory"); return NULL; }
    ps->p++;                                                     /* [ */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
    for (;;) {
        json_val *item = parse_value(ps);
        if (!item) { json_free(v); return NULL; }
        if (!grow(v, false)) { json_free(item); json_free(v); fail(ps, "out of memory"); return NULL; }
        v->items[v->n++] = item;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
        fail(ps, "expected , or ]");
        json_free(v);
        return NULL;
    }
}

static json_val *parse_object(parser *ps)
{
    json_val *v = node(J_OBJ);
    if (!v) { fail(ps, "out of memory"); return NULL; }
    ps->p++;                                                     /* { */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
    for (;;) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected object key"); json_free(v); return NULL; }
        char *key = parse_string(ps);
        if (!key) { json_free(v); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { fail(ps, "expected :"); free(key); json_free(v); return NULL; }
        ps->p++;
        json_val *val = parse_value(ps);
        if (!val) { free(key); json_free(v); return NULL; }
        if (!grow(v, true)) { free(key); json_free(val); json_free(v); fail(ps, "out of memory"); return NULL; }
        v->keys[v->n] = key;
        v->items[v->n++] = val;
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
        fail(ps, "expected , or }");
        json_free(v);
        return NULL;
    }
}

static json_val *parse_value(parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) { fail(ps, "unexpected end"); return NULL; }
    if (ps->depth >= MAX_DEPTH) { fail(ps, "nesting too deep"); return NULL; }

    char c = *ps->p;
    json_val *v = NULL;

    if (c == '{' || c == '[') {
        ps->depth++;
        v = (c == '{') ? parse_object(ps) : parse_array(ps);
        ps->depth--;
        return v;
    }
    if (c == '"') {
        char *s = parse_string(ps);
        if (!s) return NULL;
        v = node(J_STR);
        if (!v) { free(s); fail(ps, "out of memory"); return NULL; }
        v->str = s;
        return v;
    }
    if (c == 't' && ps->end - ps->p >= 4 && !strncmp(ps->p, "true", 4)) {
        ps->p += 4;
        v = node(J_BOOL);
        if (v) v->b = true;
        return v;
    }
    if (c == 'f' && ps->end - ps->p >= 5 && !strncmp(ps->p, "false", 5)) {
        ps->p += 5;
        return node(J_BOOL);
    }
    if (c == 'n' && ps->end - ps->p >= 4 && !strncmp(ps->p, "null", 4)) {
        ps->p += 4;
        return node(J_NULL);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64];
        size_t n = 0;
        while (ps->p < ps->end && n < sizeof tmp - 1 &&
               (*ps->p == '-' || *ps->p == '+' || *ps->p == '.' || *ps->p == 'e' || *ps->p == 'E' ||
                (*ps->p >= '0' && *ps->p <= '9')))
            tmp[n++] = *ps->p++;
        tmp[n] = 0;
        char *endp;
        double d = strtod(tmp, &endp);
        if (endp == tmp || *endp) { fail(ps, "bad number"); return NULL; }
        v = node(J_NUM);
        if (v) v->num = d;
        return v;
    }
    fail(ps, "unexpected character");
    return NULL;
}

json_val *json_parse(const char *text, size_t len, char *err, size_t errcap)
{
    parser ps = { text, text + len, text, 0, err, errcap, false };
    if (err && errcap) err[0] = 0;
    json_val *v = parse_value(&ps);
    if (!v) {
        if (err && errcap && !err[0]) snprintf(err, errcap, "parse error");
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        fail(&ps, "trailing data");
        json_free(v);
        return NULL;
    }
    return v;
}

const json_val *json_get(const json_val *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (size_t i = obj->n; i > 0; i--)                          /* last duplicate wins */
        if (!strcmp(obj->keys[i - 1], key)) return obj->items[i - 1];
    return NULL;
}

const char *json_str(const json_val *v, const char *def)
{
    return (v && v->type == J_STR) ? v->str : def;
}

double json_num(const json_val *v, double def)
{
    return (v && v->type == J_NUM) ? v->num : def;
}

bool json_bool(const json_val *v, bool def)
{
    return (v && v->type == J_BOOL) ? v->b : def;
}

/* =================================================================== writer */

void jw_init(jw *w)
{
    memset(w, 0, sizeof *w);
}

void jw_free(jw *w)
{
    free(w->buf);
    memset(w, 0, sizeof *w);
}

static void put(jw *w, const char *s, size_t n)
{
    if (w->oom) return;
    if (w->len + n + 1 > w->cap) {
        size_t nc = w->cap ? w->cap : 256;
        while (nc < w->len + n + 1) nc *= 2;
        char *nb = realloc(w->buf, nc);
        if (!nb) { w->oom = true; return; }
        w->buf = nb;
        w->cap = nc;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = 0;
}

static void putc_(jw *w, char c)
{
    put(w, &c, 1);
}

static void sep(jw *w)
{
    if (w->after_key) { w->after_key = false; return; }
    if (w->depth > 0) {
        if (!w->first[w->depth - 1]) putc_(w, ',');
        w->first[w->depth - 1] = false;
    }
}

static void put_escaped(jw *w, const char *s)
{
    putc_(w, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  put(w, "\\\"", 2); break;
        case '\\': put(w, "\\\\", 2); break;
        case '\n': put(w, "\\n", 2);  break;
        case '\r': put(w, "\\r", 2);  break;
        case '\t': put(w, "\\t", 2);  break;
        case '\b': put(w, "\\b", 2);  break;
        case '\f': put(w, "\\f", 2);  break;
        default:
            if (*p < 0x20) {
                char tmp[8];
                snprintf(tmp, sizeof tmp, "\\u%04x", *p);
                put(w, tmp, 6);
            } else {
                putc_(w, (char)*p);
            }
        }
    }
    putc_(w, '"');
}

static void begin(jw *w, char open)
{
    sep(w);
    putc_(w, open);
    if (w->depth < JW_MAX_DEPTH) w->first[w->depth] = true;
    w->depth++;
}

static void end(jw *w, char close)
{
    if (w->depth > 0) w->depth--;
    putc_(w, close);
}

void jw_obj(jw *w)     { begin(w, '{'); }
void jw_end_obj(jw *w) { end(w, '}'); }
void jw_arr(jw *w)     { begin(w, '['); }
void jw_end_arr(jw *w) { end(w, ']'); }

void jw_key(jw *w, const char *key)
{
    sep(w);
    put_escaped(w, key);
    putc_(w, ':');
    w->after_key = true;
}

void jw_str(jw *w, const char *s)
{
    sep(w);
    if (s) put_escaped(w, s);
    else put(w, "null", 4);
}

void jw_int(jw *w, int64_t v)
{
    char tmp[32];
    sep(w);
    int n = snprintf(tmp, sizeof tmp, "%lld", (long long)v);
    put(w, tmp, (size_t)n);
}

void jw_bool(jw *w, bool v)
{
    sep(w);
    put(w, v ? "true" : "false", v ? 4 : 5);
}

void jw_null(jw *w)
{
    sep(w);
    put(w, "null", 4);
}

void jw_kstr(jw *w, const char *key, const char *s)   { jw_key(w, key); jw_str(w, s); }
void jw_kint(jw *w, const char *key, int64_t v)       { jw_key(w, key); jw_int(w, v); }
void jw_kbool(jw *w, const char *key, bool v)         { jw_key(w, key); jw_bool(w, v); }
