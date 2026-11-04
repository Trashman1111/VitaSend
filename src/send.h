/*
 * send.h - sending files / text to another LocalSend device (the "client" half of the transfer API).
 *
 * A worker thread does: POST prepare-upload (waits for the other side to accept; asks for a PIN if
 * needed) -> POST upload for every file -> done. Progress goes to xfer.h; the UI watches that.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_SEND_H
#define LS_SEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "peers.h"

#define SEND_MAX_FILES 256

typedef struct {
    char     path[300];          /* where it is on the memory card */
    char     name[200];          /* the name announced to the receiver (may contain folders: "dir/file.txt") */
    uint64_t size;
} send_item;

/* Collects one file, or a whole folder (recursively), into the list. `list` is a growing malloc'd array.
 * Returns false if the path cannot be read; stops silently at SEND_MAX_FILES (check *n). */
bool send_collect(const char *path, send_item **list, int *n, int *cap);

/* Both return false if a transfer is already running. send_start_files() takes ownership of `items`. */
bool send_start_files(const ls_peer *peer, send_item *items, int n);
bool send_start_text(const ls_peer *peer, const char *text);

/* Retry: after a send that failed, was cancelled or was refused, the files that did NOT arrive can be sent again to the
 * same device (a new prepare-upload with only the remaining files; a half-sent file starts from its beginning). */
int  send_retry_count(void);            /* files (1 for a text message) a retry would send; 0 = nothing to retry */
bool send_retry(void);                  /* false if nothing to retry or another transfer is running */

/* PIN handshake: when the other device wants a PIN the worker waits and send_needs_pin() turns true.
 * Answer with send_give_pin(pin), or send_give_pin(NULL) to give up. */
bool send_needs_pin(void);
void send_give_pin(const char *pin);

#endif
