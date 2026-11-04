/*
 * inflate.c - raw DEFLATE decoder (own implementation, canonical Huffman decoding with a 9-bit fast table).
 *
 * Author: trashman (VitaSend)
 */
#include "inflate.h"

#include <stdlib.h>
#include <string.h>

#define WIN_SIZE  32768
#define IN_BUF    4096
#define FAST_BITS 9
#define MAXBITS   15

typedef struct {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[320];
    uint16_t fast[1 << FAST_BITS];      /* (len << 9) | symbol, 0 = not in the fast table */
} huff;

typedef struct {
    inf_read_fn  rd;
    void        *rctx;
    inf_write_fn wr;
    void        *wctx;

    uint8_t  in[IN_BUF];
    size_t   inpos, inlen;
    bool     in_eof;
    uint32_t bitbuf;
    int      bitcnt;

    uint8_t *win;
    size_t   wpos, wflush;
    uint64_t total;                     /* bytes produced so far (to validate back-references) */
    int      err;
} inf;

static const uint16_t k_len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t  k_len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t k_dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t  k_dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* ---------------------------------------------------------------- input */

static bool refill_in(inf *s)
{
    if (s->in_eof) return false;
    s->inlen = s->rd(s->rctx, s->in, IN_BUF);
    s->inpos = 0;
    if (s->inlen == 0) { s->in_eof = true; return false; }
    return true;
}

/* Tries to have at least 24 bits in the buffer; fewer only at the very end of the input. */
static void fill_bits(inf *s)
{
    while (s->bitcnt <= 24) {
        if (s->inpos == s->inlen && !refill_in(s)) return;
        s->bitbuf |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
}

static uint32_t get_bits(inf *s, int n)
{
    if (n == 0) return 0;
    if (s->bitcnt < n) fill_bits(s);
    if (s->bitcnt < n) { s->err = INF_TRUNCATED; return 0; }
    uint32_t v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

/* ---------------------------------------------------------------- output */

static bool flush_win(inf *s)
{
    if (s->wpos > s->wflush) {
        if (!s->wr(s->wctx, s->win + s->wflush, s->wpos - s->wflush)) { s->err = INF_ABORTED; return false; }
    }
    s->wflush = s->wpos;
    return true;
}

static inline void put_byte(inf *s, uint8_t b)
{
    s->win[s->wpos++] = b;
    s->total++;
    if (s->wpos == WIN_SIZE) {
        flush_win(s);
        s->wpos = 0;
        s->wflush = 0;
    }
}

/* ---------------------------------------------------------------- Huffman tables */

static int huff_build(huff *h, const uint8_t *lens, int n)
{
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) return 0;                       /* no codes at all: fine as long as nothing is decoded */

    int left = 1;
    for (int l = 1; l <= MAXBITS; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return -1;                          /* over-subscribed */
    }
    /* (an incomplete code is accepted: e.g. a single distance code) */

    uint16_t offs[MAXBITS + 2];
    uint16_t next_code[MAXBITS + 2];
    offs[1] = 0;
    next_code[1] = 0;
    uint32_t code = 0;
    for (int l = 1; l <= MAXBITS; l++) {
        offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
        code = (code + (l > 1 ? h->count[l - 1] : 0)) << 1;
        next_code[l] = (uint16_t)code;
    }
    for (int i = 0; i < n; i++) {
        int l = lens[i];
        if (!l) continue;
        h->symbol[offs[l]++] = (uint16_t)i;
        if (l <= FAST_BITS) {
            uint32_t c = next_code[l]++;
            uint32_t rev = 0;
            for (int b = 0; b < l; b++) rev |= ((c >> b) & 1u) << (l - 1 - b);
            for (uint32_t k = rev; k < (1u << FAST_BITS); k += (1u << l)) h->fast[k] = (uint16_t)((l << 9) | i);
        } else {
            next_code[l]++;
        }
    }
    return 0;
}

/* Returns the decoded symbol or -1. */
static int huff_decode(inf *s, const huff *h)
{
    if (s->bitcnt < MAXBITS) fill_bits(s);
    uint16_t e = h->fast[s->bitbuf & ((1u << FAST_BITS) - 1)];
    if (e) {
        int l = e >> 9;
        if (l > s->bitcnt) { s->err = INF_TRUNCATED; return -1; }
        s->bitbuf >>= l;
        s->bitcnt -= l;
        return e & 511;
    }
    /* slow path: canonical decoding one bit at a time */
    int code = 0, first = 0, index = 0;
    uint32_t bb = s->bitbuf;
    for (int l = 1; l <= MAXBITS; l++) {
        if (l > s->bitcnt) { s->err = INF_TRUNCATED; return -1; }
        code |= (int)(bb & 1u);
        bb >>= 1;
        int count = h->count[l];
        if (code - count < first) {
            s->bitbuf >>= l;
            s->bitcnt -= l;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = INF_CORRUPT;
    return -1;
}

/* ---------------------------------------------------------------- blocks */

static int inflate_codes(inf *s, const huff *lit, const huff *dist)
{
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return s->err ? s->err : INF_CORRUPT;
        if (sym < 256) {
            put_byte(s, (uint8_t)sym);
            if (s->err) return s->err;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return INF_CORRUPT;
            int len = k_len_base[sym] + (int)get_bits(s, k_len_extra[sym]);
            int ds = huff_decode(s, dist);
            if (ds < 0) return s->err ? s->err : INF_CORRUPT;
            if (ds >= 30) return INF_CORRUPT;
            uint32_t d = k_dist_base[ds] + get_bits(s, k_dist_extra[ds]);
            if (s->err) return s->err;
            if (d > s->total) return INF_CORRUPT;
            size_t from = (s->wpos + WIN_SIZE - d) & (WIN_SIZE - 1);
            while (len--) {
                uint8_t b = s->win[from];
                from = (from + 1) & (WIN_SIZE - 1);
                put_byte(s, b);
            }
            if (s->err) return s->err;
        }
    }
}

static int inflate_stored(inf *s)
{
    s->bitbuf >>= (s->bitcnt & 7);
    s->bitcnt -= (s->bitcnt & 7);
    uint32_t len = get_bits(s, 16);
    uint32_t nlen = get_bits(s, 16);
    if (s->err) return s->err;
    if ((len ^ 0xFFFFu) != nlen) return INF_CORRUPT;
    while (len--) {
        uint32_t b = get_bits(s, 8);
        if (s->err) return s->err;
        put_byte(s, (uint8_t)b);
        if (s->err) return s->err;
    }
    return 0;
}

static int inflate_fixed(inf *s)
{
    uint8_t l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    huff *lit = malloc(sizeof *lit), *dist = malloc(sizeof *dist);
    int r;
    if (!lit || !dist) {
        r = INF_NOMEM;
    } else {
        huff_build(lit, l, 288);
        uint8_t dl[30];
        memset(dl, 5, sizeof dl);
        huff_build(dist, dl, 30);
        r = inflate_codes(s, lit, dist);
    }
    free(lit);
    free(dist);
    return r;
}

static int inflate_dynamic(inf *s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = (int)get_bits(s, 5) + 257;
    int ndist = (int)get_bits(s, 5) + 1;
    int ncode = (int)get_bits(s, 4) + 4;
    if (s->err) return s->err;
    if (nlen > 286 || ndist > 30) return INF_CORRUPT;

    uint8_t lens[320];
    memset(lens, 0, sizeof lens);
    for (int i = 0; i < ncode; i++) lens[order[i]] = (uint8_t)get_bits(s, 3);
    if (s->err) return s->err;

    huff *lit = malloc(sizeof *lit), *dist = malloc(sizeof *dist), *cl = malloc(sizeof *cl);
    int r = 0;
    if (!lit || !dist || !cl) { r = INF_NOMEM; goto out; }
    if (huff_build(cl, lens, 19) < 0) { r = INF_CORRUPT; goto out; }

    memset(lens, 0, sizeof lens);
    for (int i = 0; i < nlen + ndist;) {
        int sym = huff_decode(s, cl);
        if (sym < 0) { r = s->err ? s->err : INF_CORRUPT; goto out; }
        if (sym < 16) {
            lens[i++] = (uint8_t)sym;
        } else {
            int rep, val = 0;
            if (sym == 16) {
                if (i == 0) { r = INF_CORRUPT; goto out; }
                val = lens[i - 1];
                rep = 3 + (int)get_bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + (int)get_bits(s, 3);
            } else {
                rep = 11 + (int)get_bits(s, 7);
            }
            if (s->err) { r = s->err; goto out; }
            if (i + rep > nlen + ndist) { r = INF_CORRUPT; goto out; }
            while (rep--) lens[i++] = (uint8_t)val;
        }
    }
    if (lens[256] == 0) { r = INF_CORRUPT; goto out; }          /* no end-of-block code */
    if (huff_build(lit, lens, nlen) < 0) { r = INF_CORRUPT; goto out; }
    if (huff_build(dist, lens + nlen, ndist) < 0) { r = INF_CORRUPT; goto out; }
    r = inflate_codes(s, lit, dist);
out:
    free(lit);
    free(dist);
    free(cl);
    return r;
}

int ls_inflate(inf_read_fn rd, void *rctx, inf_write_fn wr, void *wctx)
{
    inf *s = calloc(1, sizeof *s);
    if (!s) return INF_NOMEM;
    s->win = malloc(WIN_SIZE);
    if (!s->win) { free(s); return INF_NOMEM; }
    s->rd = rd; s->rctx = rctx; s->wr = wr; s->wctx = wctx;

    int r = 0;
    int last;
    do {
        last = (int)get_bits(s, 1);
        int type = (int)get_bits(s, 2);
        if (s->err) { r = s->err; break; }
        switch (type) {
        case 0: r = inflate_stored(s); break;
        case 1: r = inflate_fixed(s); break;
        case 2: r = inflate_dynamic(s); break;
        default: r = INF_CORRUPT; break;
        }
        if (r == 0 && s->err) r = s->err;
    } while (r == 0 && !last);

    if (r == 0 && !flush_win(s)) r = INF_ABORTED;
    free(s->win);
    free(s);
    return r;
}
