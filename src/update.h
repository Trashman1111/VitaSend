/*
 * update.h - looking for a newer VitaSend on GitHub, downloading it and checking it
 *
 *   1. update_check(): asks the GitHub API for the newest release (pre-releases count). Runs in a thread; poll update_snapshot().
 *   2. update_download(): downloads the .vpk of that release, checks its SHA-256 against the one the release publishes
 *      (SHA256SUMS.txt, or the digest GitHub itself attaches to every file) and leaves it in <data folder>/update/.
 *   3. The user interface then hands the file to the installer (app_install.c) - this file never installs anything.
 *
 * Every connection is TLS with the server's certificate checked against the roots built into the program (cacerts.c).
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_UPDATE_H
#define LS_UPDATE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UPD_IDLE = 0,
    UPD_CHECKING,
    UPD_UPTODATE,          /* the newest release is not newer than this version                */
    UPD_AVAILABLE,         /* a newer release exists: tag, size and prerelease are filled in    */
    UPD_DOWNLOADING,
    UPD_READY,             /* the package is downloaded and its checksum is right: `file`        */
    UPD_FAILED             /* `err` says why (offline, not trusted, no checksum, ...)             */
} upd_state;

typedef struct {
    upd_state state;
    bool      manual;      /* the check was started by the user (not at start-up)                */
    bool      was_download;/* FAILED: while downloading (the update is still available), not while checking */
    char      current[16]; /* the running version                                               */
    char      tag[40];     /* newest release, e.g. "v0.9.5"                                      */
    bool      prerelease;
    uint64_t  size, done;  /* the package, and how much of it has arrived                         */
    char      err[160];
    char      file[320];   /* UPD_READY: the verified package                                    */
    uint32_t  serial;      /* increases at every state change                                    */
} upd_status;

void update_init(void);
bool update_check(bool manual);          /* false if a check or download is already running */
bool update_download(void);              /* only in UPD_AVAILABLE (or UPD_FAILED after a failed download) */
void update_cancel(void);                /* stops a running check / download (the state goes back to IDLE / AVAILABLE) */
void update_snapshot(upd_status *out);
void update_reset(void);                 /* UPTODATE / FAILED / READY -> IDLE (the answer has been shown) */

/* > 0 if version string `a` is newer than `b` ("v0.9.5" and "0.9.5" both work; anything after the third number is ignored) */
int  update_version_cmp(const char *a, const char *b);

#endif
