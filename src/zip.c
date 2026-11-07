/*
 * zip.c - minimal ZIP reader.
 *
 * Author: trashman (VitaSend)
 */
#include "zip.h"

#include "inflate.h"

#include <stdlib.h>
#include <string.h>

#define MAX_CD_BYTES (24u * 1024 * 1024)

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void set_err(char *err, size_t cap, const char *msg)
{
    if (err && cap) snprintf(err, cap, "%s", msg);
}

static uint32_t s_crc_table[256];
static bool     s_crc_ready;

static void crc_init(void)
{
    if (s_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_table[i] = c;
    }
    s_crc_ready = true;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) crc = s_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t zip_crc32(const uint8_t *p, size_t n)
{
    crc_init();
    return crc_update(0, p, n);
}

void zip_close(zip_file *z)
{
    if (z->f) fclose(z->f);
    free(z->e);
    free(z->pool);
    memset(z, 0, sizeof *z);
}

bool zip_open(zip_file *z, const char *path, char *err, size_t errcap)
{
    memset(z, 0, sizeof *z);
    crc_init();
    z->f = fopen(path, "rb");
    if (!z->f) { set_err(err, errcap, "Cannot open the file"); return false; }

    if (fseek(z->f, 0, SEEK_END) != 0) { set_err(err, errcap, "Cannot read the file"); goto fail; }
    long size = ftell(z->f);
    if (size < 22) { set_err(err, errcap, "Not a VPK/ZIP file"); goto fail; }

    /* ---- end of central directory record: look for it in the last 64 KiB ---- */
    size_t tail = (size_t)(size < 65557 ? size : 65557);
    uint8_t *tb = malloc(tail);
    if (!tb) { set_err(err, errcap, "Out of memory"); goto fail; }
    if (fseek(z->f, size - (long)tail, SEEK_SET) != 0 || fread(tb, 1, tail, z->f) != tail) {
        free(tb);
        set_err(err, errcap, "Cannot read the file");
        goto fail;
    }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--) {
        if (tb[i] == 'P' && tb[i + 1] == 'K' && tb[i + 2] == 5 && tb[i + 3] == 6) { eocd = i; break; }
    }
    if (eocd < 0) { free(tb); set_err(err, errcap, "Not a VPK/ZIP file"); goto fail; }
    uint32_t nent = u16(tb + eocd + 10), cdsize = u32(tb + eocd + 12), cdoff = u32(tb + eocd + 16);
    free(tb);
    if (nent == 0xFFFF || cdsize == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu) { set_err(err, errcap, "ZIP64 archives are not supported"); goto fail; }
    if (nent == 0 || cdsize > MAX_CD_BYTES || (uint64_t)cdoff + cdsize > (uint64_t)size) { set_err(err, errcap, "Damaged archive directory"); goto fail; }

    uint8_t *cd = malloc(cdsize);
    z->e = calloc(nent, sizeof *z->e);
    z->pool = malloc((size_t)cdsize + nent + 1);       /* names are never longer than the directory itself */
    if (!cd || !z->e || !z->pool) { free(cd); set_err(err, errcap, "Out of memory"); goto fail; }
    if (fseek(z->f, (long)cdoff, SEEK_SET) != 0 || fread(cd, 1, cdsize, z->f) != cdsize) {
        free(cd);
        set_err(err, errcap, "Cannot read the archive directory");
        goto fail;
    }

    size_t pos = 0, pool_used = 0;
    for (uint32_t i = 0; i < nent; i++) {
        if (pos + 46 > cdsize || u32(cd + pos) != 0x02014b50u) { free(cd); set_err(err, errcap, "Damaged archive directory"); goto fail; }
        const uint8_t *h = cd + pos;
        uint32_t flags = u16(h + 8), method = u16(h + 10);
        uint32_t nlen = u16(h + 28), elen = u16(h + 30), clen = u16(h + 32);
        if (pos + 46 + nlen + elen + clen > cdsize) { free(cd); set_err(err, errcap, "Damaged archive directory"); goto fail; }
        if (flags & 1) { free(cd); set_err(err, errcap, "Encrypted archives are not supported"); goto fail; }

        zip_entry *e = &z->e[i];
        char *nm = z->pool + pool_used;
        for (uint32_t k = 0; k < nlen; k++) {
            char c = (char)h[46 + k];
            nm[k] = c == '\\' ? '/' : c;
        }
        nm[nlen] = 0;
        pool_used += nlen + 1;
        e->name = nm;
        e->method = method;
        e->crc = u32(h + 16);
        e->csize = u32(h + 20);
        e->usize = u32(h + 24);
        e->lho = u32(h + 42);
        e->is_dir = nlen > 0 && nm[nlen - 1] == '/';
        if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu || e->lho == 0xFFFFFFFFu) {
            free(cd);
            set_err(err, errcap, "ZIP64 archives are not supported");
            goto fail;
        }
        z->total_unpacked += e->usize;
        pos += 46 + nlen + elen + clen;
    }
    free(cd);
    z->n = (int)nent;
    return true;

fail:
    zip_close(z);
    return false;
}

int zip_find(const zip_file *z, const char *name)
{
    for (int i = 0; i < z->n; i++)
        if (strcmp(z->e[i].name, name) == 0) return i;
    return -1;
}

bool zip_name_is_safe(const char *name)
{
    if (!name[0] || name[0] == '/') return false;
    for (const char *p = name; *p; p++)
        if (*p == ':' || (unsigned char)*p < 32) return false;
    const char *seg = name;
    for (;;) {
        const char *end = strchr(seg, '/');
        size_t len = end ? (size_t)(end - seg) : strlen(seg);
        if (len == 2 && seg[0] == '.' && seg[1] == '.') return false;
        if (!end) break;
        seg = end + 1;
    }
    return true;
}

const char *zip_strerror(int code)
{
    switch (code) {
    case ZIP_OK: case ZIP_STOPPED: return "ok";
    case ZIP_BAD: return "The archive is damaged";
    case ZIP_IO: return "Read or write error";
    case ZIP_CRC: return "A file is damaged (checksum mismatch)";
    case ZIP_UNSUPPORTED: return "Unsupported compression method";
    default: return "Unknown error";
    }
}

/* ---------------------------------------------------------------- extraction */

typedef struct {
    zip_file *z;
    uint32_t  left;                 /* compressed bytes still to read */
    zip_sink  sink;
    void     *sctx;
    uint32_t  crc;
    uint64_t  out;
    uint64_t  want;
    bool      io_error;
    bool      stopped;
    bool      overrun;
} xstate;

static size_t x_read(void *ctx, uint8_t *buf, size_t cap)
{
    xstate *x = ctx;
    if (x->left == 0) return 0;
    size_t n = cap < x->left ? cap : x->left;
    size_t got = fread(buf, 1, n, x->z->f);
    if (got != n) x->io_error = true;
    x->left -= (uint32_t)got;
    return got;
}

static bool x_write(void *ctx, const uint8_t *data, size_t len)
{
    xstate *x = ctx;
    if (x->out + len > x->want) { x->overrun = true; return false; }
    x->crc = crc_update(x->crc, data, len);
    x->out += len;
    if (!x->sink(x->sctx, data, len)) { x->stopped = true; return false; }
    return true;
}

int zip_extract(zip_file *z, int idx, zip_sink sink, void *ctx)
{
    if (idx < 0 || idx >= z->n) return ZIP_BAD;
    const zip_entry *e = &z->e[idx];
    uint8_t lh[30];
    if (fseek(z->f, (long)e->lho, SEEK_SET) != 0 || fread(lh, 1, 30, z->f) != 30) return ZIP_IO;
    if (u32(lh) != 0x04034b50u) return ZIP_BAD;
    long data = (long)e->lho + 30 + u16(lh + 26) + u16(lh + 28);
    if (fseek(z->f, data, SEEK_SET) != 0) return ZIP_IO;

    xstate x = { .z = z, .left = e->csize, .sink = sink, .sctx = ctx, .crc = 0, .want = e->usize };
    if (e->method == 0) {
        if (e->csize != e->usize) return ZIP_BAD;
        uint8_t *buf = malloc(32768);
        if (!buf) return ZIP_IO;
        int r = ZIP_OK;
        for (;;) {
            size_t n = x_read(&x, buf, 32768);
            if (n == 0) break;
            if (!x_write(&x, buf, n)) { r = ZIP_STOPPED; break; }
        }
        free(buf);
        if (x.io_error) return ZIP_IO;
        if (r == ZIP_STOPPED) return ZIP_STOPPED;
    } else if (e->method == 8) {
        int r = ls_inflate(x_read, &x, x_write, &x);
        if (x.stopped) return ZIP_STOPPED;
        if (x.io_error) return ZIP_IO;
        if (r != INF_OK || x.overrun) return ZIP_BAD;
    } else {
        return ZIP_UNSUPPORTED;
    }
    if (x.out != e->usize) return ZIP_BAD;
    if (x.crc != e->crc) return ZIP_CRC;
    return ZIP_OK;
}
