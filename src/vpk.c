/*
 * vpk.c - see vpk.h.
 *
 * Author: trashman (VitaSend)
 */
#include "vpk.h"

#include "headbin.h"
#include "plat.h"
#include "util.h"
#include "zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAFE_AUTHID    0x2F00000000000002ull
#define PATH_CAP       512
#define WRITE_BUF      (64 * 1024)
#define FREE_MARGIN    (4ull * 1024 * 1024)

/* ============================================================== helpers */

bool vpk_has_extension(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && ls_strcasecmp(name + n - 4, ".vpk") == 0;
}

static void rm_tree(const char *path, int depth)
{
    plat_dirent *list = NULL;
    int n = plat_list_dir(path, &list);
    if (n < 0) return;
    for (int i = 0; i < n; i++) {
        char child[PATH_CAP];
        ls_path_join(child, sizeof child, path, list[i].name);
        if (list[i].is_dir) {
            if (depth < 40) rm_tree(child, depth + 1);
        } else {
            plat_remove(child);
        }
    }
    free(list);
    plat_rmdir(path);
}

/* ---- param.sfo ---- */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

static bool sfo_string(const uint8_t *d, size_t len, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    if (len < 20 || memcmp(d, "\0PSF", 4) != 0) return false;
    uint32_t ko = le32(d + 8), dof = le32(d + 12), n = le32(d + 16);
    for (uint32_t i = 0; i < n && 20 + (size_t)i * 16 + 16 <= len; i++) {
        const uint8_t *e = d + 20 + (size_t)i * 16;
        uint32_t k = le16(e), fmt = le16(e + 2), dlen = le32(e + 4), doff = le32(e + 12);
        if ((size_t)ko + k >= len || (size_t)dof + doff > len) continue;
        const char *name = (const char *)d + ko + k;
        if (strnlen(name, len - ko - k) >= len - ko - k || strcmp(name, key) != 0) continue;
        if (fmt != 0x0004 && fmt != 0x0204) return false;       /* not a string */
        size_t avail = len - dof - doff;
        if (dlen > avail) dlen = (uint32_t)avail;
        size_t m = strnlen((const char *)d + dof + doff, dlen);
        if (m >= cap) m = cap - 1;
        memcpy(out, d + dof + doff, m);
        out[m] = 0;
        return true;
    }
    return false;
}

static bool valid_title_id(const char *t)
{
    if (strlen(t) != 9) return false;
    for (int i = 0; i < 9; i++)
        if (!((t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9'))) return false;
    return true;
}

/* ---- reading a few bytes out of an entry ---- */

typedef struct {
    uint8_t *buf;
    size_t   cap, len;
    bool     stop_when_full;
} grab;

static bool grab_sink(void *ctx, const uint8_t *data, size_t n)
{
    grab *g = ctx;
    size_t room = g->cap - g->len;
    size_t take = n < room ? n : room;
    memcpy(g->buf + g->len, data, take);
    g->len += take;
    return !(g->stop_when_full && g->len >= g->cap);
}

/* ============================================================== inspect */

static bool inspect_open(zip_file *z, vpk_info *info)
{
    int sfo_i = zip_find(z, "sce_sys/param.sfo");
    if (sfo_i < 0) { snprintf(info->err, sizeof info->err, "Not a Vita package (sce_sys/param.sfo is missing)"); return false; }
    int eb_i = zip_find(z, "eboot.bin");
    if (eb_i < 0) { snprintf(info->err, sizeof info->err, "Not an app package (eboot.bin is missing)"); return false; }
    if (z->e[sfo_i].usize == 0 || z->e[sfo_i].usize > 65536) { snprintf(info->err, sizeof info->err, "param.sfo looks damaged"); return false; }

    uint8_t *sfo = malloc(z->e[sfo_i].usize);
    if (!sfo) { snprintf(info->err, sizeof info->err, "Out of memory"); return false; }
    grab g = { sfo, z->e[sfo_i].usize, 0, false };
    int r = zip_extract(z, sfo_i, grab_sink, &g);
    if (r != ZIP_OK) {
        free(sfo);
        snprintf(info->err, sizeof info->err, "%s", zip_strerror(r));
        return false;
    }
    sfo_string(sfo, g.len, "TITLE_ID", info->titleid, sizeof info->titleid);
    sfo_string(sfo, g.len, "TITLE", info->title, sizeof info->title);
    sfo_string(sfo, g.len, "APP_VER", info->version, sizeof info->version);
    sfo_string(sfo, g.len, "CONTENT_ID", info->content_id, sizeof info->content_id);
    free(sfo);
    for (char *c = info->title; *c; c++)
        if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
    if (!info->title[0]) ls_strlcpy(info->title, info->titleid, sizeof info->title);

    if (!valid_title_id(info->titleid)) { snprintf(info->err, sizeof info->err, "The title id in param.sfo is invalid"); return false; }

    /* extended permissions? The SELF header of eboot.bin carries the "auth id" at 0x80. */
    uint8_t hdr[0x88];
    grab h = { hdr, sizeof hdr, 0, true };
    memset(hdr, 0, sizeof hdr);
    r = zip_extract(z, eb_i, grab_sink, &h);
    if (r != ZIP_OK && r != ZIP_STOPPED) { snprintf(info->err, sizeof info->err, "eboot.bin: %s", zip_strerror(r)); return false; }
    if (h.len >= sizeof hdr && memcmp(hdr, "SCE\0", 4) == 0) {
        uint64_t authid = (uint64_t)le32(hdr + 0x80) | ((uint64_t)le32(hdr + 0x84) << 32);
        info->unsafe = authid != SAFE_AUTHID;
    }

    for (int i = 0; i < z->n; i++)
        if (!z->e[i].is_dir) info->files++;
    info->unpacked = z->total_unpacked;

    if (strcmp(info->titleid, plat_own_title_id()) == 0) {
        snprintf(info->err, sizeof info->err, "This is VitaSend itself - it cannot replace itself while it is running");
        return false;
    }
    return true;
}

bool vpk_inspect(const char *path, vpk_info *info)
{
    memset(info, 0, sizeof *info);
    zip_file z;
    if (!zip_open(&z, path, info->err, sizeof info->err)) return false;
    info->ok = inspect_open(&z, info);
    zip_close(&z);
    return info->ok;
}

/* ============================================================== the installation thread */

static plat_mutex *s_mx;
static vpk_status  s_st;
static volatile bool s_cancel;

static void st_set(vpk_state state, const char *stage, int pct, const char *msg)
{
    plat_mutex_lock(s_mx);
    if (state != (vpk_state)-1) s_st.state = state;
    if (stage) ls_strlcpy(s_st.stage, stage, sizeof s_st.stage);
    if (pct >= 0) s_st.pct = pct;
    if (msg) ls_strlcpy(s_st.message, msg, sizeof s_st.message);
    plat_mutex_unlock(s_mx);
}

void vpk_init(void)
{
    s_mx = plat_mutex_create();
}

bool vpk_busy(void)
{
    plat_mutex_lock(s_mx);
    bool b = s_st.state == VPK_RUNNING;
    plat_mutex_unlock(s_mx);
    return b;
}

void vpk_snapshot(vpk_status *out)
{
    plat_mutex_lock(s_mx);
    *out = s_st;
    plat_mutex_unlock(s_mx);
}

void vpk_cancel(void)
{
    s_cancel = true;
}

typedef struct {
    FILE    *f;
    uint8_t *buf;
    size_t   len;
    uint64_t done, total;
    int      last_pct;
    bool     werr;
} unpack;

static bool unpack_flush(unpack *u)
{
    if (u->len && u->f && fwrite(u->buf, 1, u->len, u->f) != u->len) { u->werr = true; return false; }
    u->len = 0;
    return true;
}

static bool unpack_sink(void *ctx, const uint8_t *data, size_t n)
{
    unpack *u = ctx;
    if (s_cancel) return false;
    u->done += n;
    while (n) {
        size_t room = WRITE_BUF - u->len;
        size_t take = n < room ? n : room;
        memcpy(u->buf + u->len, data, take);
        u->len += take;
        data += take;
        n -= take;
        if (u->len == WRITE_BUF && !unpack_flush(u)) return false;
    }
    int pct = u->total ? (int)(u->done * 90 / u->total) : 0;
    if (pct != u->last_pct) { u->last_pct = pct; st_set((vpk_state)-1, NULL, pct, NULL); }
    return true;
}

static void fail(const char *fmt, const char *a)
{
    char m[200];
    snprintf(m, sizeof m, fmt, a);
    st_set(VPK_FAILED, NULL, -1, m);
}

static void install_thread(void *arg)
{
    char *file = arg;
    const char *pkg = plat_pkg_dir();
    zip_file z;
    memset(&z, 0, sizeof z);
    unpack u;
    memset(&u, 0, sizeof u);
    u.buf = malloc(WRITE_BUF);
    char err[160] = "";
    char path[PATH_CAP], parent[PATH_CAP] = "";
    char why[160];
    vpk_info info;
    memset(&info, 0, sizeof info);

    plat_keep_awake();
    st_set(VPK_RUNNING, "Checking the package", 0, "");
    rm_tree(pkg, 0);

    if (!u.buf) { fail("%s", "Out of memory"); goto done; }
    if (!zip_open(&z, file, err, sizeof err)) { fail("%s", err); goto done; }
    if (!inspect_open(&z, &info)) { fail("%s", info.err); goto done; }
    plat_mutex_lock(s_mx);
    ls_strlcpy(s_st.title, info.title, sizeof s_st.title);
    ls_strlcpy(s_st.titleid, info.titleid, sizeof s_st.titleid);
    plat_mutex_unlock(s_mx);

    uint64_t have = plat_free_space(pkg);
    if (have != UINT64_MAX && have < info.unpacked + FREE_MARGIN) {
        char m[200];
        snprintf(m, sizeof m, "Not enough space on the memory card: %llu MB needed, %llu MB free",
                 (unsigned long long)((info.unpacked + FREE_MARGIN) >> 20), (unsigned long long)(have >> 20));
        st_set(VPK_FAILED, NULL, -1, m);
        goto done;
    }

    st_set(VPK_RUNNING, "Unpacking", 0, NULL);
    u.total = info.unpacked ? info.unpacked : 1;
    u.last_pct = -1;
    if (!plat_mkdirs(pkg)) { fail("Cannot create %s", pkg); goto done; }

    for (int i = 0; i < z.n; i++) {
        const zip_entry *e = &z.e[i];
        if (!zip_name_is_safe(e->name)) { fail("Unsafe file name in the package: %s", e->name); goto done; }
        if (strlen(pkg) + 1 + strlen(e->name) >= sizeof path) { fail("File name too long: %s", e->name); goto done; }
        ls_path_join(path, sizeof path, pkg, e->name);
        if (e->is_dir) {
            path[strlen(path) - 1] = 0;
            plat_mkdirs(path);
            continue;
        }
        char *slash = strrchr(path, '/');
        if (slash) {
            *slash = 0;
            if (strcmp(parent, path) != 0) {
                plat_mkdirs(path);
                ls_strlcpy(parent, path, sizeof parent);
            }
            *slash = '/';
        }
        u.f = fopen(path, "wb");
        if (!u.f) { fail("Cannot create %s", e->name); goto done; }
        u.len = 0;
        int r = zip_extract(&z, i, unpack_sink, &u);
        bool flushed = r == ZIP_OK ? unpack_flush(&u) : true;
        bool closed = fclose(u.f) == 0;
        u.f = NULL;
        if (s_cancel) { st_set(VPK_CANCELLED, NULL, -1, "Cancelled - nothing was installed"); goto done; }
        if (u.werr || !flushed || !closed) { st_set(VPK_FAILED, NULL, -1, "Write error (is the memory card full?)"); goto done; }
        if (r != ZIP_OK) {
            char m[200];
            snprintf(m, sizeof m, "%s: %s", e->name, zip_strerror(r));
            st_set(VPK_FAILED, NULL, -1, m);
            goto done;
        }
        plat_keep_awake();
    }
    zip_close(&z);
    memset(&z, 0, sizeof z);

    /* head.bin - unless the package brings its own */
    st_set(VPK_RUNNING, "Preparing", 92, NULL);
    plat_mutex_lock(s_mx);
    s_st.unpacked = true;
    plat_mutex_unlock(s_mx);
    snprintf(path, sizeof path, "%s/sce_sys/package/head.bin", pkg);
    bool bf;
    {
        bool is_dir = false;
        uint64_t sz = 0;
        bf = plat_path_info(path, &is_dir, &sz) && !is_dir && sz > 0;
    }
    if (!bf) {
        uint8_t head[LS_HEAD_BIN_SIZE];
        char dir[PATH_CAP];
        snprintf(dir, sizeof dir, "%s/sce_sys/package", pkg);
        plat_mkdirs(dir);
        ls_make_head_bin(info.titleid, info.content_id, head);
        if (!ls_file_write(path, head, sizeof head)) { st_set(VPK_FAILED, NULL, -1, "Cannot write head.bin"); goto done; }
    }

    if (s_cancel) { st_set(VPK_CANCELLED, NULL, -1, "Cancelled - nothing was installed"); goto done; }

    st_set(VPK_RUNNING, "Installing (this takes a moment)", 95, NULL);
    plat_keep_awake();
    if (plat_promote(pkg, why, sizeof why) != 0) {
        st_set(VPK_FAILED, NULL, -1, why);
        goto done;
    }
    {
        char m[200];
        snprintf(m, sizeof m, "%s is installed. Look for its bubble on the LiveArea.", info.title);
        st_set(VPK_DONE, "Done", 100, m);
    }

done:
    if (u.f) fclose(u.f);
    zip_close(&z);
    free(u.buf);
    rm_tree(pkg, 0);
    free(file);
}

bool vpk_start(const char *path)
{
    plat_mutex_lock(s_mx);
    if (s_st.state == VPK_RUNNING) { plat_mutex_unlock(s_mx); return false; }
    uint32_t serial = s_st.serial + 1;
    memset(&s_st, 0, sizeof s_st);
    s_st.serial = serial;
    s_st.state = VPK_RUNNING;
    ls_strlcpy(s_st.stage, "Starting", sizeof s_st.stage);
    plat_mutex_unlock(s_mx);

    s_cancel = false;
    char *copy = malloc(strlen(path) + 1);
    if (!copy) { st_set(VPK_FAILED, NULL, -1, "Out of memory"); return false; }
    strcpy(copy, path);
    if (!plat_thread_spawn("vpk-install", install_thread, copy, 256 * 1024)) {
        free(copy);
        st_set(VPK_FAILED, NULL, -1, "Cannot start the installer thread");
        return false;
    }
    return true;
}
