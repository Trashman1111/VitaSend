/*
 * inflate.h - a small, self-contained raw DEFLATE (RFC 1951) decoder, so the VPK installer needs no zlib.
 * Streaming: input is pulled through a callback, output is pushed through another one in chunks.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_INFLATE_H
#define LS_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { INF_OK = 0, INF_CORRUPT = -1, INF_TRUNCATED = -2, INF_ABORTED = -3, INF_NOMEM = -4 };

/* Reads up to `cap` bytes; returns the count, 0 at the end of the compressed data (or on a read error). */
typedef size_t (*inf_read_fn)(void *ctx, uint8_t *buf, size_t cap);
/* Receives decompressed bytes. Return false to stop (-> INF_ABORTED). */
typedef bool (*inf_write_fn)(void *ctx, const uint8_t *buf, size_t len);

int ls_inflate(inf_read_fn rd, void *rctx, inf_write_fn wr, void *wctx);

#endif
