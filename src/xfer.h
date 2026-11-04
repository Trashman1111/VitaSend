/*
 * xfer.h - the one transfer (sending OR receiving) that can run at a time, and its progress.
 * The engines (rx.c, send.c) update it from their threads; the UI only reads snapshots.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_XFER_H
#define LS_XFER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { XF_IDLE = 0, XF_WAITING, XF_ACTIVE, XF_DONE, XF_FAILED, XF_CANCELLED } xf_state;

typedef struct {
    bool     outgoing;
    xf_state state;
    char     peer[64];
    char     current[96];        /* file being transferred */
    int      files_total, files_done;
    uint64_t bytes_total, bytes_done;
    uint32_t speed_bps;          /* smoothed */
    char     message[160];       /* what is going on / why it failed */
    uint32_t serial;             /* changes whenever a new transfer begins */
} xfer_status;

void xfer_init(void);                       /* once, before any thread that might use this module */

/* Returns false if another transfer is in progress. */
bool xfer_begin(bool outgoing, const char *peer);
bool xfer_busy(void);
void xfer_snapshot(xfer_status *out);

void xfer_set_message(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void xfer_set_state(xf_state st);
void xfer_set_totals(int files, uint64_t bytes);
void xfer_set_current(const char *name);
void xfer_add_bytes(uint64_t n);
void xfer_file_done(void);

/* Ends the transfer; the final state and message stay visible until the next one begins. */
void xfer_end(xf_state final_state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Ends it without leaving anything to show (e.g. the user rejected a request). */
void xfer_end_silent(void);

/* The user pressed "cancel". Engines poll this flag (and hand its address to the HTTP layer). */
void           xfer_request_cancel(void);
volatile bool *xfer_cancel_flag(void);

#endif
