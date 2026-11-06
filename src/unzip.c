/*
 * unzip.c - unpacking a .zip file into a new folder (see unzip.h)
 *
 * Author: trashman (VitaSend)
 */
#include "unzip.h"

#include "plat.h"
#include "util.h"
#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UZ_PATH     512
#define UZ_BUF      (64 * 1024)
#define UZ_MARGIN   (4u << 20)               /* room to keep free besides the unpacked files */
#define UZ_DEPTH    24

static plat_mutex   *s_mx;
static unzip_status  s_st;
static volatile bool s_cancel;

bool unzip_has_extension(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && ls_strcasecmp(name + n - 4, ".zip") == 0;
}

static int path_depth(const char *name)
{
    int d = 0;
    for (const char *p = name; *p; p++) if (*p == '/') d++;
    return d;
}

/* Everything that is not a plain stored / deflated file or folder is refused up front, so that no half-way surprise happens. */
static bool check_entries(const zip_file *z, unzip_info *info)
{
    if (z->n == 0) { snprintf(info->err, sizeof info->err, "The zip file is empty"); return false; }
    for (int i = 0; i < z->n; i++) {
        const zip_entry *e = &z->e[i];
        if (!zip_name_is_safe(e->name)) { snprintf(info->err, sizeof info->err, "Unsafe file name inside: %.60s", e->name); return false; }
        if (path_depth(e->name) > UZ_DEPTH) { snprintf(info->err, sizeof info->err, "Folders nested too deep inside"); return false; }
        if (strlen(e->name) > 200) { snprintf(info->err, sizeof info->err, "A file name inside is too long"); return false; }
        if (!e->is_dir && e->method != 0 && e->method != 8) { snprintf(info->err, sizeof info->err, "%.60s: unsupported compression (only stored and deflate)", e->name); return false; }
        if (e->is_dir) info->dirs++; else info->files++;
    }
    info->unpacked = z->total_unpacked;
    return true;
}

bool unzip_inspect(const char *zip_path, unzip_info *info)
{
    memset(info, 0, sizeof *info);
    zip_file z;
    if (!zip_open(&z, zip_path, info->err, sizeof info->err)) return false;
    info->ok = check_entries(&z, info);
    zip_close(&z);
    return info->ok;
}

/* ---- the thread ---- */

static void set_msg(unzip_state st, const char *msg)
{
    plat_mutex_lock(s_mx);
    s_st.state = st;
    if (msg) ls_strlcpy(s_st.message, msg, sizeof s_st.message);
    s_st.serial++;
    plat_mutex_unlock(s_mx);
}

static void rm_tree(const char *path, int depth)
{
    plat_dirent *v = NULL;
    int n = plat_list_dir(path, &v);
    if (n >= 0) {
        for (int i = 0; i < n; i++) {
            char sub[UZ_PATH + 8];
            snprintf(sub, sizeof sub, "%s/%s", path, v[i].name);
            if (v[i].is_dir) { if (depth < UZ_DEPTH + 8) rm_tree(sub, depth + 1); }
            else plat_remove(sub);
        }
        free(v);
    }
    plat_rmdir(path);
}

typedef struct { FILE *f; uint8_t *buf; size_t len; bool werr; } outbuf;

static bool out_flush(outbuf *o)
{
    if (o->len && o->f && fwrite(o->buf, 1, o->len, o->f) != o->len) { o->werr = true; return false; }
    o->len = 0;
    return true;
}

static bool out_sink(void *ctx, const uint8_t *data, size_t n)
{
    outbuf *o = ctx;
    if (s_cancel) return false;
    plat_mutex_lock(s_mx);
    s_st.done += n;
    plat_mutex_unlock(s_mx);
    while (n) {
        size_t take = UZ_BUF - o->len < n ? UZ_BUF - o->len : n;
        memcpy(o->buf + o->len, data, take);
        o->len += take; data += take; n -= take;
        if (o->len == UZ_BUF && !out_flush(o)) return false;
    }
#ifdef LS_HOST
    static int slow = -1;
    if (slow < 0) { const char *e = getenv("LS_SLOW_UNZIP_MS"); slow = e ? atoi(e) : 0; }
    if (slow > 0) plat_sleep_ms((unsigned)slow);             /* tests: slow enough to be cancelled */
#endif
    return true;
}

typedef struct { char zip[UZ_PATH]; char dest[UZ_PATH]; } job_arg;

static void unzip_thread(void *arg)
{
    job_arg *ja = arg;
    zip_file z;
    memset(&z, 0, sizeof z);
    outbuf o = { 0, malloc(UZ_BUF), 0, false };
    unzip_info info;
    memset(&info, 0, sizeof info);
    bool created = false;
    char parent[UZ_PATH] = "";

    plat_keep_awake();
    if (!o.buf) { set_msg(UZ_FAILED, "Out of memory"); goto done; }
    if (!zip_open(&z, ja->zip, info.err, sizeof info.err) || !check_entries(&z, &info)) { set_msg(UZ_FAILED, info.err); goto done; }
    uint64_t have = plat_free_space(ja->dest);
    if (have != UINT64_MAX && have < info.unpacked + UZ_MARGIN) {
        char m[200];
        snprintf(m, sizeof m, "Not enough space on the memory card: %llu MB needed, %llu MB free",
                 (unsigned long long)((info.unpacked + UZ_MARGIN) >> 20), (unsigned long long)(have >> 20));
        set_msg(UZ_FAILED, m);
        goto done;
    }
    plat_mutex_lock(s_mx);
    s_st.files_total = info.files;
    s_st.total = info.unpacked;
    plat_mutex_unlock(s_mx);
    if (!plat_mkdirs(ja->dest)) { set_msg(UZ_FAILED, "Cannot create the folder"); goto done; }
    created = true;

    for (int i = 0; i < z.n; i++) {
        const zip_entry *e = &z.e[i];
        char path[UZ_PATH + 220];
        snprintf(path, sizeof path, "%s/%s", ja->dest, e->name);
        if (strlen(path) >= 290) { set_msg(UZ_FAILED, "A path inside is too long for the memory card"); goto done; }   /* (PATH_CAP of the file browser is 300) */
        if (s_cancel) { set_msg(UZ_CANCELLED, "Cancelled - nothing was unpacked"); goto done; }
        if (e->is_dir) {
            path[strlen(path) - 1] = 0;
            plat_mkdirs(path);
            continue;
        }
        char *slash = strrchr(path, '/');
        if (slash) {
            *slash = 0;
            if (strcmp(parent, path) != 0) { plat_mkdirs(path); ls_strlcpy(parent, path, sizeof parent); }
            *slash = '/';
        }
        plat_mutex_lock(s_mx);
        const char *base = strrchr(e->name, '/');
        ls_strlcpy(s_st.cur, base ? base + 1 : e->name, sizeof s_st.cur);
        plat_mutex_unlock(s_mx);
        o.f = fopen(path, "wb");
        if (!o.f) { char m[200]; snprintf(m, sizeof m, "Cannot create %.100s", e->name); set_msg(UZ_FAILED, m); goto done; }
        o.len = 0;
        int r = zip_extract(&z, i, out_sink, &o);
        bool flushed = r == ZIP_OK ? out_flush(&o) : true;
        bool closed = fclose(o.f) == 0;
        o.f = NULL;
        if (s_cancel) { set_msg(UZ_CANCELLED, "Cancelled - nothing was unpacked"); goto done; }
        if (o.werr || !flushed || !closed) { set_msg(UZ_FAILED, "Write error (is the memory card full?)"); goto done; }
        if (r != ZIP_OK) {
            char m[200];
            snprintf(m, sizeof m, "%.100s: %s", e->name, zip_strerror(r));
            set_msg(UZ_FAILED, m);
            goto done;
        }
        plat_mutex_lock(s_mx);
        s_st.files_done++;
        plat_mutex_unlock(s_mx);
        plat_keep_awake();
    }
    ls_log("unzip: %d file(s), %llu bytes -> %s", info.files, (unsigned long long)info.unpacked, ja->dest);
    set_msg(UZ_DONE, "");
    created = false;                          /* keep it */

done:
    if (o.f) fclose(o.f);
    zip_close(&z);
    free(o.buf);
    if (created) rm_tree(ja->dest, 0);        /* failed or cancelled: either everything is there or nothing */
    free(ja);
}

void unzip_init(void) { s_mx = plat_mutex_create(); }

bool unzip_busy(void)
{
    plat_mutex_lock(s_mx);
    bool b = s_st.state == UZ_RUNNING;
    plat_mutex_unlock(s_mx);
    return b;
}

void unzip_snapshot(unzip_status *out)
{
    plat_mutex_lock(s_mx);
    *out = s_st;
    plat_mutex_unlock(s_mx);
}

void unzip_cancel(void) { s_cancel = true; }

bool unzip_start(const char *zip_path, const char *dest_dir)
{
    if (strlen(zip_path) >= UZ_PATH || strlen(dest_dir) >= UZ_PATH) return false;
    plat_mutex_lock(s_mx);
    if (s_st.state == UZ_RUNNING) { plat_mutex_unlock(s_mx); return false; }
    uint32_t serial = s_st.serial + 1;
    memset(&s_st, 0, sizeof s_st);
    s_st.serial = serial;
    s_st.state = UZ_RUNNING;
    plat_mutex_unlock(s_mx);
    s_cancel = false;
    job_arg *ja = calloc(1, sizeof *ja);
    if (!ja) { set_msg(UZ_FAILED, "Out of memory"); return false; }
    ls_strlcpy(ja->zip, zip_path, sizeof ja->zip);
    ls_strlcpy(ja->dest, dest_dir, sizeof ja->dest);
    if (!plat_thread_spawn("unzip", unzip_thread, ja, 256 * 1024)) {
        free(ja);
        set_msg(UZ_FAILED, "Cannot start the unpacking thread");
        return false;
    }
    return true;
}
