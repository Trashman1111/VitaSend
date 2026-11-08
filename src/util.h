/*
 * util.h - Constants (name, version, default port, multicast address, limits) and the small helpers of util.c.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_UTIL_H
#define LS_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_APP_NAME      "VitaSend"
#define LS_APP_AUTHOR    "trashman"
#define LS_APP_VERSION   "0.9.8"
#define LS_APP_DATE      "2026-11-07"       /* release date, shown in Settings > About */
/* A line under the credit in Settings > About - the place for "support the developer" information. Empty = not shown.
 * (Build with -DLS_SUPPORT_TEXT='"..."' to try one without editing this file.) */
#ifndef LS_SUPPORT_TEXT
#define LS_SUPPORT_TEXT  ""
#endif
#define LS_HELPER_TITLE_ID "LSNDUPD01"     /* the small updater app VitaSend installs for a self-update (9 characters, like LS_TITLE_ID) */
#define LS_PROTO_VERSION "2.1"            /* what we claim in "version" */
#define LS_DEFAULT_PORT  53317
#define LS_MULTICAST_IP  "224.0.0.167"

#ifndef LS_PRINTF
#define LS_PRINTF(a, b) __attribute__((format(printf, a, b)))
#endif

extern volatile bool g_quit;              /* set to true to make every thread wind down */

/* Call once, first thing in main(). */
void ls_util_init(void);

/* ---- logging: goes to the on-screen log, a file (if set) and stdout (host build) ---- */
#define LS_LOG_LINES     64
#define LS_LOG_LINE_LEN  200
void ls_log(const char *fmt, ...) LS_PRINTF(1, 2);
void ls_log_set_file(const char *path);
/* Copies the newest lines (oldest first) into out, returns how many. */
int  ls_log_tail(char out[][LS_LOG_LINE_LEN], int max_lines);
uint32_t ls_log_counter(void);            /* increases whenever a line is added */

/* ---- threads (detached, but counted so shutdown can wait for them) ---- */
typedef void (*ls_thread_fn)(void *arg);
bool ls_spawn(const char *name, ls_thread_fn fn, void *arg, size_t stack_bytes);
int  ls_active_threads(void);
bool ls_wait_threads(unsigned timeout_ms);

/* ---- small helpers ---- */
void   ls_hex(const uint8_t *in, size_t n, char *out);       /* out needs 2n+1 bytes */
size_t ls_strlcpy(char *dst, const char *src, size_t cap);
int    ls_strcasecmp(const char *a, const char *b);
int    ls_strncasecmp(const char *a, const char *b, size_t n);
bool   ls_file_read(const char *path, uint8_t **data, size_t *len);   /* malloc'd, +1 NUL */
bool   ls_file_write(const char *path, const void *data, size_t len);
void   ls_path_join(char *out, size_t cap, const char *dir, const char *name);

#endif
