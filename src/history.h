/*
 * history.h - the list of finished transfers and received text messages (<data dir>/history.json).
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_HISTORY_H
#define LS_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#define HIST_MAX      40
#define HIST_TEXT_MAX 1024

typedef enum { H_OK = 0, H_FAILED, H_CANCELLED, H_REJECTED } hist_status;

typedef struct {
    int64_t     time;                 /* unix seconds (UTC), 0 if the Vita clock was unavailable */
    bool        outgoing;
    hist_status status;
    bool        is_text;
    char        peer[64];
    int         nfiles;
    uint64_t    bytes;
    char        summary[96];          /* first file name, or the start of the message */
    char        text[HIST_TEXT_MAX];  /* the message (is_text only) */
} hist_entry;

void history_load(const char *dir);
void history_add(const hist_entry *e);              /* thread safe; saves to disk */
int  history_count(void);
bool history_get(int index_newest_first, hist_entry *out);

#endif
