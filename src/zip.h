/*
 * zip.h - minimal ZIP reader (stored + deflate, no encryption, no ZIP64) - enough for .vpk files.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_ZIP_H
#define LS_ZIP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *name;           /* '/' separated, directories end with '/' */
    uint32_t    method;         /* 0 = stored, 8 = deflate */
    uint32_t    crc;
    uint32_t    csize, usize;
    uint32_t    lho;            /* offset of the local header */
    bool        is_dir;
} zip_entry;

typedef struct {
    FILE      *f;
    zip_entry *e;
    int        n;
    char      *pool;            /* all names */
    uint64_t   total_unpacked;
} zip_file;

/* Opens the archive and reads its directory. On failure `err` says why. */
bool zip_open(zip_file *z, const char *path, char *err, size_t errcap);
void zip_close(zip_file *z);
int  zip_find(const zip_file *z, const char *name);            /* index or -1 */

/* Receives unpacked bytes; return false to stop early. */
typedef bool (*zip_sink)(void *ctx, const uint8_t *data, size_t len);
enum { ZIP_OK = 0, ZIP_STOPPED = 1, ZIP_BAD = -1, ZIP_IO = -2, ZIP_CRC = -3, ZIP_UNSUPPORTED = -4 };
/* Unpacks one entry through `sink`. ZIP_STOPPED if the sink stopped it (the CRC is then not checked). */
int  zip_extract(zip_file *z, int idx, zip_sink sink, void *ctx);
const char *zip_strerror(int code);

/* A file name from inside an archive is safe to use below a folder: no "..", no absolute path, no drive letter. */
bool zip_name_is_safe(const char *name);

#endif
