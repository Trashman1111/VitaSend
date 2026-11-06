/*
 * unzip.h - unpacking an ordinary .zip file into a new folder (Files tab). Runs in a thread; the user interface polls
 * unzip_snapshot(). Built on zip.c / inflate.c (stored + deflate, no passwords, no ZIP64).
 *
 * A failed or cancelled unpacking removes the folder it created: either everything is there or nothing is.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_UNZIP_H
#define LS_UNZIP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     ok;
    char     err[128];
    int      files, dirs;
    uint64_t unpacked;
} unzip_info;

bool unzip_has_extension(const char *name);               /* ends in ".zip" (any case) */
bool unzip_inspect(const char *zip_path, unzip_info *info);   /* reads the directory only; false + info->err if it cannot be unpacked */

typedef enum { UZ_IDLE = 0, UZ_RUNNING, UZ_DONE, UZ_FAILED, UZ_CANCELLED } unzip_state;

typedef struct {
    unzip_state state;
    int         files_done, files_total;
    uint64_t    done, total;            /* bytes */
    char        cur[96];                /* the file being unpacked */
    char        message[200];           /* the error, when FAILED */
    uint32_t    serial;
} unzip_status;

void unzip_init(void);
bool unzip_start(const char *zip_path, const char *dest_dir);   /* dest_dir must not exist yet; false if one is running */
bool unzip_busy(void);
void unzip_snapshot(unzip_status *out);
void unzip_cancel(void);

#endif
