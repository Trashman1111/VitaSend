/*
 * vpk.h - installing .vpk packages on the Vita (the "VitaShell way"):
 *
 *   1. unpack the VPK (a ZIP file) into ux0:data/pkg        (zip.c / inflate.c, with progress and cancel)
 *   2. write sce_sys/package/head.bin                       (headbin.c)
 *   3. hand the folder to the system installer              (plat_promote)
 *
 * vpk_inspect() looks inside a package without unpacking it; vpk_start() runs the whole thing in a thread that the UI
 * polls through vpk_snapshot().
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_VPK_H
#define LS_VPK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     ok;                 /* looks installable                                           */
    char     err[128];           /* if !ok: why                                                 */
    char     title[96];
    char     titleid[16];
    char     version[16];
    char     content_id[48];
    bool     unsafe;             /* the app asks for extended permissions (the system may warn) */
    uint64_t unpacked;           /* bytes after unpacking                                       */
    int      files;
} vpk_info;

bool vpk_has_extension(const char *name);           /* ends in ".vpk" (any case) */
bool vpk_inspect(const char *path, vpk_info *info); /* reads only a few KiB; fills info->err when it returns false */

typedef enum { VPK_IDLE = 0, VPK_RUNNING, VPK_DONE, VPK_FAILED, VPK_CANCELLED } vpk_state;

typedef struct {
    vpk_state state;
    char      stage[64];         /* "Unpacking", "Installing"...                 */
    int       pct;               /* 0..100                                       */
    char      message[200];      /* final result or the error                    */
    char      title[96];
    char      titleid[16];       /* e.g. "ABCD12345" - what plat_launch_app() opens */
    bool      unpacked;          /* the package has been unpacked (what follows is the system installer) */
    uint32_t  serial;            /* changes when a new installation starts       */
} vpk_status;

void vpk_init(void);
bool vpk_start(const char *path);        /* false if one is already running */
bool vpk_busy(void);
void vpk_snapshot(vpk_status *out);
void vpk_cancel(void);                   /* only honoured while unpacking */

#endif
