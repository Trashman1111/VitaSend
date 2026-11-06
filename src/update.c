/*
 * update.c - looking for a newer VitaSend on GitHub, downloading it and checking it (see update.h)
 *
 * A small HTTPS client of its own: one GET per connection, redirects followed (GitHub serves release files from another host),
 * bodies with Content-Length or chunked, streamed to a callback. Every connection checks the server's certificate against the
 * built-in roots. The download is only accepted when its SHA-256 matches the checksum the release publishes.
 *
 * Author: trashman (VitaSend)
 */
#include "update.h"

#include "json.h"
#include "plat.h"
#include "tls.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPD_REPO        "Trashman1111/vitasend"
#define UPD_API_HOST    "api.github.com"
#define UPD_STACK       (256 * 1024)
#define UPD_MAX_JSON    (768 * 1024)         /* the list of releases */
#define UPD_MAX_SUMS    (64 * 1024)
#define UPD_MAX_VPK     (48u * 1024 * 1024)
#define HTTP_HDR_MAX    16384
#define UPD_IDLE_MS     20000                /* a transfer that makes no progress for this long fails */

static plat_mutex   *s_lock;
static upd_status    s_st;
static char          s_vpk_url[1024];          /* what the check found, used by the download */
static char          s_vpk_name[96];
static char          s_sums_url[1024];
static char          s_digest[65];            /* sha256 of the .vpk from the release data, "" if GitHub gave none */
static volatile bool s_cancel;
static volatile bool s_busy;

static void lock(void)   { plat_mutex_lock(s_lock); }
static void unlock(void) { plat_mutex_unlock(s_lock); }

void update_init(void)
{
    s_lock = plat_mutex_create();
    memset(&s_st, 0, sizeof s_st);
    ls_strlcpy(s_st.current, LS_APP_VERSION, sizeof s_st.current);
}

void update_snapshot(upd_status *out)
{
    lock();
    *out = s_st;
    unlock();
}

static void fail(const char *fmt, ...) LS_PRINTF(1, 2);
static void fail(const char *fmt, ...)
{
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ls_log("Update: %s", msg);
    lock();
    ls_strlcpy(s_st.err, msg, sizeof s_st.err);
    s_st.state = s_cancel ? (s_st.was_download ? UPD_AVAILABLE : UPD_IDLE) : UPD_FAILED;   /* the user gave up: no error to show */
    s_st.serial++;
    unlock();
}

void update_reset(void)
{
    lock();
    if (s_st.state == UPD_UPTODATE || s_st.state == UPD_FAILED || s_st.state == UPD_READY) {
        s_st.state = UPD_IDLE;
        s_st.serial++;
    }
    unlock();
}

/* ============================================================== versions */

static bool parse_version(const char *s, int v[3])
{
    while (*s == 'v' || *s == 'V') s++;
    v[0] = v[1] = v[2] = 0;
    int n = 0;
    while (n < 3 && *s >= '0' && *s <= '9') {
        v[n++] = atoi(s);
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '.') s++; else break;
    }
    return n > 0;
}

int update_version_cmp(const char *a, const char *b)
{
    int x[3], y[3];
    if (!parse_version(a, x) || !parse_version(b, y)) return 0;
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
    return 0;
}

/* ============================================================== HTTPS GET */

typedef bool (*sink_fn)(void *ctx, const uint8_t *p, size_t n);   /* false = stop */

typedef struct { char host[96]; uint16_t port; char path[2300]; } ls_url;

static bool url_parse(const char *s, ls_url *u)
{
    if (strncmp(s, "https://", 8) != 0) return false;           /* a plain http address is never followed */
    s += 8;
    const char *slash = strchr(s, '/');
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    char hp[130];
    if (hl == 0 || hl >= sizeof hp) return false;
    memcpy(hp, s, hl);
    hp[hl] = 0;
    u->port = 443;
    char *colon = strchr(hp, ':');
    if (colon) { *colon = 0; u->port = (uint16_t)atoi(colon + 1); if (!u->port) return false; }
    if (!hp[0] || strlen(hp) >= sizeof u->host) return false;
    ls_strlcpy(u->host, hp, sizeof u->host);
    if (strlen(slash ? slash : "/") >= sizeof u->path) return false;
    ls_strlcpy(u->path, slash ? slash : "/", sizeof u->path);
    return true;
}

static bool hdr_get(const char *hdrs, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *p = hdrs; *p; ) {
        const char *e = strstr(p, "\r\n");
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll > nl && p[nl] == ':' && ls_strncasecmp(p, name, nl) == 0) {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = (size_t)(p + ll - v);
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            return true;
        }
        if (!e) break;
        p = e + 2;
    }
    return false;
}

typedef struct { int st; int64_t left; char line[24]; int ll; } chunk_dec;   /* st: 0 size line, 1 data, 2 CRLF, 3 finished */

/* returns 0 = go on, 1 = the body is complete, -1 = error / the sink said stop */
static int chunk_feed(chunk_dec *d, const uint8_t *p, size_t n, sink_fn sink, void *ctx)
{
    while (n > 0 && d->st != 3) {
        if (d->st == 0) {
            uint8_t c = *p++; n--;
            if (c == '\n') {
                d->line[d->ll] = 0;
                char *semi = strchr(d->line, ';');
                if (semi) *semi = 0;
                char *end = NULL;
                long long sz = strtoll(d->line, &end, 16);
                if (end == d->line || sz < 0) return -1;
                d->ll = 0;
                if (sz == 0) { d->st = 3; return 1; }
                d->left = sz;
                d->st = 1;
            } else if (c != '\r') {
                if (d->ll >= (int)sizeof d->line - 1) return -1;
                d->line[d->ll++] = (char)c;
            }
        } else if (d->st == 1) {
            size_t k = (int64_t)n < d->left ? n : (size_t)d->left;
            if (!sink(ctx, p, k)) return -1;
            p += k; n -= k; d->left -= (int64_t)k;
            if (d->left == 0) d->st = 2;
        } else {                                  /* st 2: the CRLF after a chunk */
            uint8_t c = *p++; n--;
            if (c == '\n') d->st = 0;
        }
    }
    return d->st == 3 ? 1 : 0;
}

static const char *api_host_override(void)
{
#ifdef LS_HOST
    return getenv("LS_UPDATE_API");               /* tests: "localhost:PORT" instead of api.github.com */
#else
    return NULL;
#endif
}

/* Fetches an https address (following redirects) into `sink`. `api` adds the GitHub API Accept header. */
static bool https_get(const char *url, bool api, sink_fn sink, void *ctx, uint64_t *content_len, char *err, size_t ecap)
{
    ls_url u;
    if (!url_parse(url, &u)) { snprintf(err, ecap, "Bad address"); return false; }
    if (content_len) *content_len = 0;
    for (int hop = 0; hop < 6; hop++) {
        if (s_cancel) { snprintf(err, ecap, "Cancelled"); return false; }
        char ip[16];
        if (!plat_resolve(u.host, ip, 8000)) { snprintf(err, ecap, "Cannot find %s - is Wi-Fi connected to the internet?", u.host); return false; }
        char why[160];
        conn_t *c = conn_connect_verified(ip, u.port, u.host, 15000, why, sizeof why);
        if (!c) { snprintf(err, ecap, "%s", why[0] ? why : "Could not connect"); return false; }

        char req[3000];
        char hostline[120];
        if (u.port == 443) snprintf(hostline, sizeof hostline, "%s", u.host);
        else snprintf(hostline, sizeof hostline, "%s:%u", u.host, (unsigned)u.port);
        int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: VitaSend/%s\r\nAccept: %s\r\nConnection: close\r\n\r\n",
                          u.path, hostline, LS_APP_VERSION, api ? "application/vnd.github+json" : "*/*");
        if (rl <= 0 || rl >= (int)sizeof req || conn_write_all(c, req, (size_t)rl) != 0) {
            conn_close(c);
            snprintf(err, ecap, "The request could not be sent");
            return false;
        }

        /* headers */
        static char hbuf[HTTP_HDR_MAX + 1];       /* (only one update thread runs at a time) */
        size_t hn = 0;
        char *hend = NULL;
        uint64_t last = plat_time_ms();
        while (!hend) {
            if (s_cancel) { conn_close(c); snprintf(err, ecap, "Cancelled"); return false; }
            if (hn >= HTTP_HDR_MAX) break;
            int r = conn_read(c, hbuf + hn, HTTP_HDR_MAX - hn);
            if (r > 0) {
                hn += (size_t)r;
                hbuf[hn] = 0;
                hend = strstr(hbuf, "\r\n\r\n");
                last = plat_time_ms();
            } else if (r == LS_IO_TIMEOUT) {
                if (plat_time_ms() - last > UPD_IDLE_MS) break;
            } else break;
        }
        if (!hend) { conn_close(c); snprintf(err, ecap, "No answer from %s", u.host); return false; }
        *hend = 0;
        size_t body_off = (size_t)(hend - hbuf) + 4;
        int status = 0;
        if (sscanf(hbuf, "HTTP/%*d.%*d %d", &status) != 1) { conn_close(c); snprintf(err, ecap, "Unexpected answer from %s", u.host); return false; }
        const char *hdrs = strstr(hbuf, "\r\n");
        hdrs = hdrs ? hdrs + 2 : "";

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char loc[2400];
            if (!hdr_get(hdrs, "Location", loc, sizeof loc)) { conn_close(c); snprintf(err, ecap, "Redirect without a target"); return false; }
            conn_close(c);
            ls_url nu;
            if (loc[0] == '/') {                                 /* same server */
                nu = u;
                if (strlen(loc) >= sizeof nu.path) { snprintf(err, ecap, "Redirect target too long"); return false; }
                ls_strlcpy(nu.path, loc, sizeof nu.path);
            } else if (!url_parse(loc, &nu)) {
                snprintf(err, ecap, "The server redirected to an address that is not https");
                return false;
            }
            u = nu;
            continue;
        }
        if (status != 200) {
            conn_close(c);
            if (status == 403 || status == 429) snprintf(err, ecap, "GitHub says: too many requests (HTTP %d) - try again later", status);
            else if (status == 404) snprintf(err, ecap, "Not found on GitHub (HTTP 404)");
            else snprintf(err, ecap, "GitHub answered HTTP %d", status);
            return false;
        }

        char tmp[64];
        bool chunked = hdr_get(hdrs, "Transfer-Encoding", tmp, sizeof tmp) && strstr(tmp, "chunked") != NULL;
        int64_t clen = -1;
        if (!chunked && hdr_get(hdrs, "Content-Length", tmp, sizeof tmp)) clen = atoll(tmp);
        if (content_len && clen > 0) *content_len = (uint64_t)clen;
        if (!chunked && clen == 0) { conn_close(c); return true; }       /* an empty body */

        chunk_dec cd = { 0 };
        int64_t got = 0;
        bool done = false, ok = true;
        /* what came with the headers, then the rest */
        static uint8_t rbuf[16384];
        size_t pre = hn - body_off;
        const uint8_t *chunk = (const uint8_t *)hbuf + body_off;
        size_t cn = pre;
        last = plat_time_ms();
        for (;;) {
            if (cn > 0) {
                if (chunked) {
                    int r = chunk_feed(&cd, chunk, cn, sink, ctx);
                    if (r < 0) { ok = false; break; }
                    if (r == 1) { done = true; break; }
                } else {
                    size_t k = cn;
                    if (clen >= 0 && got + (int64_t)k > clen) k = (size_t)(clen - got);
                    if (k && !sink(ctx, chunk, k)) { ok = false; break; }
                    got += (int64_t)k;
                    if (clen >= 0 && got >= clen) { done = true; break; }
                }
            }
            if (s_cancel) { ok = false; snprintf(err, ecap, "Cancelled"); break; }
            int r = conn_read(c, rbuf, sizeof rbuf);
            if (r > 0) { chunk = rbuf; cn = (size_t)r; last = plat_time_ms(); continue; }
            cn = 0;
            if (r == LS_IO_TIMEOUT) {
                if (plat_time_ms() - last > UPD_IDLE_MS) { ok = false; snprintf(err, ecap, "The download stalled"); break; }
                continue;
            }
            if (r == 0 && !chunked && clen < 0) { done = true; break; }       /* the server closed: that is the end */
            ok = false;
            snprintf(err, ecap, "The connection was interrupted");
            break;
        }
        conn_close(c);
        if (ok && done) return true;
        if (!err[0]) snprintf(err, ecap, "The answer was cut off or refused");
        return false;
    }
    snprintf(err, ecap, "Too many redirects");
    return false;
}

/* ---- sinks ---- */

typedef struct { uint8_t *buf; size_t len, cap, max; } mem_sink;

static bool mem_write(void *ctx, const uint8_t *p, size_t n)
{
    mem_sink *m = ctx;
    if (m->len + n > m->max) return false;
    if (m->len + n + 1 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 16384;
        while (nc < m->len + n + 1) nc *= 2;
        uint8_t *nb = realloc(m->buf, nc);
        if (!nb) return false;
        m->buf = nb;
        m->cap = nc;
    }
    memcpy(m->buf + m->len, p, n);
    m->len += n;
    m->buf[m->len] = 0;
    return true;
}

typedef struct { FILE *f; ls_sha256 *h; uint64_t done, max; bool werr; } file_sink;

static bool file_write(void *ctx, const uint8_t *p, size_t n)
{
    file_sink *s = ctx;
    if (s->done + n > s->max) return false;
    if (fwrite(p, 1, n, s->f) != n) { s->werr = true; return false; }
    sha256_update(s->h, p, n);
    s->done += n;
    lock();
    s_st.done = s->done;
    unlock();
    return true;
}

/* ============================================================== the check */

static bool ends_with_nocase(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && ls_strcasecmp(s + a - b, suffix) == 0;
}

static bool hex64(const char *s)
{
    for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    return true;
}

static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }

static void check_thread(void *arg)
{
    bool manual = arg != NULL;
    char err[160] = "";
    char url[300];
    const char *ov = api_host_override();
    if (ov && *ov) snprintf(url, sizeof url, "https://%s/repos/%s/releases?per_page=6", ov, UPD_REPO);
    else snprintf(url, sizeof url, "https://%s/repos/%s/releases?per_page=6", UPD_API_HOST, UPD_REPO);

    int roots = tls_roots_usable();
    ls_log("Update: looking for a newer version (this is %s, %d trusted roots)", LS_APP_VERSION, roots);

    mem_sink m = { 0, 0, 0, UPD_MAX_JSON };
    if (!https_get(url, true, mem_write, &m, NULL, err, sizeof err)) {
        free(m.buf);
        fail("%s", err[0] ? err : "Could not reach GitHub");
        s_busy = false;
        return;
    }
    char jerr[80];
    json_val *j = json_parse((const char *)m.buf, m.len, jerr, sizeof jerr);
    free(m.buf);
    if (!j || j->type != J_ARR) {
        json_free(j);
        fail("GitHub's answer could not be read");
        s_busy = false;
        return;
    }

    /* the highest version among the published releases (a pre-release counts) */
    const json_val *best = NULL;
    char best_tag[40] = "";
    bool best_pre = false;
    for (size_t i = 0; i < j->n; i++) {
        const json_val *r = j->items[i];
        if (!r || r->type != J_OBJ || json_bool(json_get(r, "draft"), false)) continue;
        const char *tag = json_str(json_get(r, "tag_name"), "");
        int v[3];
        if (!parse_version(tag, v)) continue;
        if (!best || update_version_cmp(tag, best_tag) > 0) {
            best = r;
            ls_strlcpy(best_tag, tag, sizeof best_tag);
            best_pre = json_bool(json_get(r, "prerelease"), false);
        }
    }
    if (!best) {
        json_free(j);
        fail("No release was found on GitHub");
        s_busy = false;
        return;
    }
    if (update_version_cmp(best_tag, LS_APP_VERSION) <= 0) {
        json_free(j);
        ls_log("Update: %s is the newest version (latest release: %s)", LS_APP_VERSION, best_tag);
        lock();
        ls_strlcpy(s_st.tag, best_tag, sizeof s_st.tag);
        s_st.manual = manual;
        s_st.state = s_cancel ? UPD_IDLE : UPD_UPTODATE;
        s_st.serial++;
        unlock();
        s_busy = false;
        return;
    }

    /* its package and checksum */
    char vpk_url[1024] = "", vpk_name[96] = "", sums_url[1024] = "", digest[65] = "";
    uint64_t vpk_size = 0;
    const json_val *as = json_get(best, "assets");
    for (size_t i = 0; as && as->type == J_ARR && i < as->n; i++) {
        const json_val *a = as->items[i];
        const char *nm = json_str(json_get(a, "name"), "");
        const char *dl = json_str(json_get(a, "browser_download_url"), "");
        if (ends_with_nocase(nm, ".vpk") && !vpk_url[0]) {
            ls_strlcpy(vpk_url, dl, sizeof vpk_url);
            ls_strlcpy(vpk_name, nm, sizeof vpk_name);
            vpk_size = (uint64_t)json_num(json_get(a, "size"), 0);
            const char *dg = json_str(json_get(a, "digest"), "");
            if (strncmp(dg, "sha256:", 7) == 0 && hex64(dg + 7)) { ls_strlcpy(digest, dg + 7, sizeof digest); lower(digest); }
        } else if (ls_strcasecmp(nm, "SHA256SUMS.txt") == 0 && !sums_url[0]) {
            ls_strlcpy(sums_url, dl, sizeof sums_url);
        }
    }
    json_free(j);
    if (!vpk_url[0]) { fail("Release %s has no .vpk attached", best_tag); s_busy = false; return; }

    lock();
    ls_strlcpy(s_vpk_url, vpk_url, sizeof s_vpk_url);
    ls_strlcpy(s_vpk_name, vpk_name, sizeof s_vpk_name);
    ls_strlcpy(s_sums_url, sums_url, sizeof s_sums_url);
    ls_strlcpy(s_digest, digest, sizeof s_digest);
    ls_strlcpy(s_st.tag, best_tag, sizeof s_st.tag);
    s_st.prerelease = best_pre;
    s_st.size = vpk_size;
    s_st.done = 0;
    s_st.manual = manual;
    s_st.state = s_cancel ? UPD_IDLE : UPD_AVAILABLE;
    s_st.serial++;
    unlock();
    ls_log("Update: %s%s is available (%s, %llu bytes)", best_tag, best_pre ? " (pre-release)" : "", vpk_name, (unsigned long long)vpk_size);
    s_busy = false;
}

bool update_check(bool manual)
{
    if (!s_lock || s_busy) return false;
    lock();
    if (s_st.state == UPD_DOWNLOADING) { unlock(); return false; }
    s_st.state = UPD_CHECKING;
    s_st.manual = manual;
    s_st.was_download = false;
    s_st.err[0] = 0;
    s_st.serial++;
    unlock();
    s_cancel = false;
    s_busy = true;
    if (!ls_spawn("ls_update", check_thread, (void *)(intptr_t)(manual ? 1 : 0), UPD_STACK)) {
        s_busy = false;
        fail("Cannot start the update check");
        return false;
    }
    return true;
}

/* ============================================================== the download */

/* "hash  name" or "hash *name" lines, as sha256sum writes them */
static bool find_sum(const char *text, const char *name, char out[65])
{
    for (const char *p = text; *p; ) {
        const char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        if (ll > 66 && hex64(p)) {
            const char *n = p + 64;
            while (n < p + ll && (*n == ' ' || *n == '\t' || *n == '*')) n++;
            size_t nl = (size_t)(p + ll - n);
            while (nl && (n[nl - 1] == '\r' || n[nl - 1] == ' ')) nl--;
            if (nl == strlen(name) && strncmp(n, name, nl) == 0) {
                memcpy(out, p, 64);
                out[64] = 0;
                lower(out);
                return true;
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static void download_thread(void *arg)
{
    (void)arg;
    char err[160] = "";
    char vpk_url[1024], sums_url[1024], vpk_name[96], digest[65], tag[40];
    uint64_t size;
    lock();
    ls_strlcpy(vpk_url, s_vpk_url, sizeof vpk_url);
    ls_strlcpy(sums_url, s_sums_url, sizeof sums_url);
    ls_strlcpy(vpk_name, s_vpk_name, sizeof vpk_name);
    ls_strlcpy(digest, s_digest, sizeof digest);
    ls_strlcpy(tag, s_st.tag, sizeof tag);
    size = s_st.size;
    unlock();

    /* 1. the checksum the release publishes (SHA256SUMS.txt first, else GitHub's own digest of the file) */
    char want[65] = "";
    if (sums_url[0]) {
        mem_sink m = { 0, 0, 0, UPD_MAX_SUMS };
        if (https_get(sums_url, false, mem_write, &m, NULL, err, sizeof err) && m.buf) {
            if (!find_sum((const char *)m.buf, vpk_name, want)) ls_log("Update: SHA256SUMS.txt has no line for %s", vpk_name);
        } else {
            ls_log("Update: SHA256SUMS.txt could not be fetched (%s)", err);
            err[0] = 0;
        }
        free(m.buf);
    }
    if (!want[0] && digest[0]) { ls_strlcpy(want, digest, sizeof want); ls_log("Update: using the digest GitHub lists for the file"); }
    if (!want[0]) {
        lock(); s_st.was_download = true; unlock();
        fail("This release publishes no checksum, so the download is not trusted - install it with VitaShell");
        s_busy = false;
        return;
    }
    if (s_cancel) goto cancelled;

    /* 2. the package */
    char dir[300], part[360], final[340];
    ls_path_join(dir, sizeof dir, plat_data_dir(), "update");
    plat_mkdirs(dir);
    char safe[48];
    size_t k = 0;
    for (const char *t = tag; *t && k < sizeof safe - 1; t++)
        safe[k++] = ((*t >= '0' && *t <= '9') || (*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') || *t == '.' || *t == '-') ? *t : '_';
    safe[k] = 0;
    char fname[96];
    snprintf(fname, sizeof fname, "VitaSend-%s.vpk", safe);
    ls_path_join(final, sizeof final, dir, fname);
    snprintf(part, sizeof part, "%s.part", final);
    plat_remove(part);
    if (size && plat_free_space(dir) < size + (2u << 20)) {
        lock(); s_st.was_download = true; unlock();
        fail("Not enough free space on the memory card for the download");
        s_busy = false;
        return;
    }

    file_sink fs = { fopen(part, "wb"), sha256_new(), 0, size ? size + 4096 : UPD_MAX_VPK, false };
    if (!fs.f || !fs.h) {
        if (fs.f) fclose(fs.f);
        sha256_discard(fs.h);
        lock(); s_st.was_download = true; unlock();
        fail("Cannot write %s", part);
        s_busy = false;
        return;
    }
    uint64_t clen = 0;
    bool ok = https_get(vpk_url, false, file_write, &fs, &clen, err, sizeof err);
    if (fclose(fs.f) != 0) fs.werr = true;
    char got[65];
    sha256_finish_hex(fs.h, got);
    if (!ok || fs.werr) {
        plat_remove(part);
        if (s_cancel) goto cancelled;
        lock(); s_st.was_download = true; unlock();
        fail("%s", fs.werr ? "Writing the file failed (memory card full?)" : (err[0] ? err : "The download failed"));
        s_busy = false;
        return;
    }
    if (size && fs.done != size) {
        plat_remove(part);
        lock(); s_st.was_download = true; unlock();
        fail("The file has the wrong size (%llu of %llu bytes)", (unsigned long long)fs.done, (unsigned long long)size);
        s_busy = false;
        return;
    }
    if (strcmp(got, want) != 0) {
        plat_remove(part);
        ls_log("Update: checksum mismatch: got %s, release says %s", got, want);
        lock(); s_st.was_download = true; unlock();
        fail("The checksum does not match - the file was deleted");
        s_busy = false;
        return;
    }
    plat_remove(final);
    if (!plat_rename(part, final)) {
        plat_remove(part);
        lock(); s_st.was_download = true; unlock();
        fail("Cannot save the downloaded file");
        s_busy = false;
        return;
    }
    ls_log("Update: %s downloaded and verified (SHA-256 %.16s...)", fname, got);
    lock();
    ls_strlcpy(s_st.file, final, sizeof s_st.file);
    s_st.state = UPD_READY;
    s_st.serial++;
    unlock();
    s_busy = false;
    return;

cancelled:
    ls_log("Update: cancelled");
    lock();
    s_st.state = UPD_AVAILABLE;                  /* the update is still there; nothing was kept */
    s_st.done = 0;
    s_st.serial++;
    unlock();
    s_busy = false;
}

bool update_download(void)
{
    if (!s_lock || s_busy) return false;
    lock();
    bool ok = s_st.state == UPD_AVAILABLE || (s_st.state == UPD_FAILED && s_st.was_download);
    if (ok) {
        s_st.state = UPD_DOWNLOADING;
        s_st.done = 0;
        s_st.err[0] = 0;
        s_st.serial++;
    }
    unlock();
    if (!ok) return false;
    s_cancel = false;
    s_busy = true;
    if (!ls_spawn("ls_download", download_thread, NULL, UPD_STACK)) {
        s_busy = false;
        fail("Cannot start the download");
        return false;
    }
    return true;
}

void update_cancel(void)
{
    s_cancel = true;
    lock();
    if (s_st.state == UPD_CHECKING) {                  /* the thread notices the flag when the connection answers or fails */
        s_st.state = UPD_IDLE;
        s_st.serial++;
    }
    unlock();
}
