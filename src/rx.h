/*
 * rx.h - receiving files and text messages (the "server" half of the LocalSend transfer API):
 *
 *   POST /api/localsend/v2/prepare-upload   -> asks the user (or auto-accepts), hands out tokens
 *   POST /api/localsend/v2/upload           -> streams one file to the memory card
 *   POST /api/localsend/v2/cancel
 *
 * The HTTP threads call the rx_handle_* functions; the UI thread polls rx_pending()/rx_tick().
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_RX_H
#define LS_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "history.h"
#include "http.h"

#define RX_MAX_FILES 256

typedef struct {
    char     alias[64];
    char     model[32];
    char     ip[16];
    char     fp[65];                     /* fingerprint, "" if unknown */
    int      nfiles;
    uint64_t total;
    bool     is_text;
    char     text[HIST_TEXT_MAX];        /* the message, if is_text */
    char     names[4][96];               /* the first few file names */
    int      nexist;                     /* how many of the files already exist in the receive folder */
    int      on_exists;                  /* what will happen to them (EXISTS_* from settings.h) */
} rx_request;

void rx_init(void);                      /* once, before the HTTP server starts */

/* ---- UI side ---- */
bool rx_pending(rx_request *out);        /* true while a sender waits for our answer */
void rx_decide(bool accept);
/* Paths of the .vpk files received in the latest session (finished ones only); returns how many were copied. */
#define RX_PATH_CAP 520
int  rx_received_vpks(char out[][RX_PATH_CAP], int max);
bool rx_received_from_favorite(void);   /* was the sender of the latest session one of the favorites? */
bool rx_take_message(char *from, size_t from_cap, char *text, size_t text_cap);   /* a text message arrived */
void rx_tick(void);                      /* call often: session time-outs, user cancel */

/* ---- HTTP side ---- */
void rx_handle_prepare(http_req *req, http_res *res);
void rx_handle_upload(http_req *req, http_res *res);
void rx_handle_cancel(http_req *req, http_res *res);

/* exposed for the self tests */
void rx_sanitize_path(const char *in, char *out, size_t cap);

#endif
