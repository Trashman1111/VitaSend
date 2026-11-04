/*
 * util.c - Small helpers: logging (ring buffer + log.txt), thread bookkeeping, string and path helpers, whole-file read / write.
 *
 * Author: trashman (VitaSend)
 */
#include "util.h"
#include "plat.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile bool g_quit = false;

/* ------------------------------------------------------------------ logging */

static plat_mutex *s_log_lock;
static char        s_lines[LS_LOG_LINES][LS_LOG_LINE_LEN];
static unsigned    s_line_count;      /* total lines ever written */
static FILE       *s_log_file;
static uint64_t    s_t0;

static plat_mutex *s_thr_lock;
static int         s_threads;

void ls_util_init(void)
{
    s_log_lock = plat_mutex_create();
    s_thr_lock = plat_mutex_create();
    s_t0 = plat_time_ms();
}

void ls_log_set_file(const char *path)
{
    if (s_log_file) {
        fclose(s_log_file);
        s_log_file = NULL;
    }
    if (path)
        s_log_file = fopen(path, "a");
}

void ls_log(const char *fmt, ...)
{
    char msg[LS_LOG_LINE_LEN - 12];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* strip trailing newline(s) and any non-printable bytes */
    for (char *p = msg; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f)
            *p = ' ';

    uint64_t t = plat_time_ms() - s_t0;
    char line[LS_LOG_LINE_LEN];
    snprintf(line, sizeof line, "[%4u.%03u] %s", (unsigned)(t / 1000), (unsigned)(t % 1000), msg);

    if (s_log_lock) plat_mutex_lock(s_log_lock);
    ls_strlcpy(s_lines[s_line_count % LS_LOG_LINES], line, LS_LOG_LINE_LEN);
    s_line_count++;
    if (s_log_file) {
        fputs(line, s_log_file);
        fputc('\n', s_log_file);
        fflush(s_log_file);
    }
#ifdef LS_HOST
    puts(line);
    fflush(stdout);
#endif
    if (s_log_lock) plat_mutex_unlock(s_log_lock);
}

int ls_log_tail(char out[][LS_LOG_LINE_LEN], int max_lines)
{
    int n = 0;
    if (s_log_lock) plat_mutex_lock(s_log_lock);
    unsigned have = s_line_count < LS_LOG_LINES ? s_line_count : LS_LOG_LINES;
    unsigned take = have < (unsigned)max_lines ? have : (unsigned)max_lines;
    for (unsigned i = 0; i < take; i++) {
        unsigned idx = (s_line_count - take + i) % LS_LOG_LINES;
        ls_strlcpy(out[n++], s_lines[idx], LS_LOG_LINE_LEN);
    }
    if (s_log_lock) plat_mutex_unlock(s_log_lock);
    return n;
}

uint32_t ls_log_counter(void)
{
    return s_line_count;
}

/* ------------------------------------------------------------------ threads */

typedef struct {
    ls_thread_fn fn;
    void        *arg;
} spawn_ctx;

static void spawn_tramp(void *p)
{
    spawn_ctx c = *(spawn_ctx *)p;
    free(p);
    c.fn(c.arg);
    plat_mutex_lock(s_thr_lock);
    s_threads--;
    plat_mutex_unlock(s_thr_lock);
}

bool ls_spawn(const char *name, ls_thread_fn fn, void *arg, size_t stack_bytes)
{
    spawn_ctx *c = malloc(sizeof *c);
    if (!c) return false;
    c->fn = fn;
    c->arg = arg;
    plat_mutex_lock(s_thr_lock);
    s_threads++;
    plat_mutex_unlock(s_thr_lock);
    if (!plat_thread_spawn(name, spawn_tramp, c, stack_bytes)) {
        plat_mutex_lock(s_thr_lock);
        s_threads--;
        plat_mutex_unlock(s_thr_lock);
        free(c);
        return false;
    }
    return true;
}

int ls_active_threads(void)
{
    plat_mutex_lock(s_thr_lock);
    int n = s_threads;
    plat_mutex_unlock(s_thr_lock);
    return n;
}

bool ls_wait_threads(unsigned timeout_ms)
{
    uint64_t end = plat_time_ms() + timeout_ms;
    while (ls_active_threads() > 0) {
        if (plat_time_ms() >= end) return false;
        plat_sleep_ms(20);
    }
    return true;
}

/* ------------------------------------------------------------------ helpers */

void ls_hex(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = 0;
}

size_t ls_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) {
        size_t c = n >= cap ? cap - 1 : n;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

int ls_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int ls_strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
        a++;
        b++;
    }
    return 0;
}

bool ls_file_read(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return false;
    }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return false;
    }
    buf[sz] = 0;
    *data = buf;
    *len = (size_t)sz;
    return true;
}

bool ls_file_write(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t put = fwrite(data, 1, len, f);
    int rc = fclose(f);
    return put == len && rc == 0;
}

void ls_path_join(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s/%s", dir, name);
}
