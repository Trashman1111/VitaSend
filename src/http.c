/*
 * http.c - A small HTTP/1.1 server and client on top of conn_t (TLS or plain): request parsing, Content-Length and chunked bodies,
 * 100-continue, streaming of big bodies straight to a route handler. See http.h for the interface.
 *
 * Author: trashman (VitaSend)
 */
#include "http.h"
#include "plat.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_DEADLINE_MS 10000
#define BODY_DEADLINE_MS   20000
#define IDLE_TIMEOUT_MS    5000
#define CONN_STACK_BYTES   (256 * 1024)

/* ============================================================== small helpers */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *status_text(int s)
{
    switch (s) {
    case 100: return "Continue";
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    default:  return "Internal Server Error";
    }
}

/* Finds "\r\n\r\n". Returns the index of the first \r or -1. */
static long find_header_end(const uint8_t *b, size_t len)
{
    if (len < 4) return -1;
    for (size_t i = 0; i + 3 < len; i++)
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') return (long)i;
    return -1;
}

/* Looks up a header in a "Name: value\r\n" block, case-insensitive. */
static bool hdr_get(const char *hdrs, const char *name, char *out, size_t cap)
{
    size_t nlen = strlen(name);
    const char *p = hdrs;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t line = eol ? (size_t)(eol - p) : strlen(p);
        if (line > nlen && p[nlen] == ':' && !ls_strncasecmp(p, name, nlen)) {
            const char *v = p + nlen + 1;
            const char *end = p + line;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
            size_t n = (size_t)(end - v);
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }
        p = eol ? eol + 2 : NULL;
    }
    return false;
}

/* ============================================================== chunked decoding */

/* 1 = complete (out/out_len filled), 0 = need more input, -1 = malformed / too big */
static int chunked_decode(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t pos = 0, o = 0;
    for (;;) {
        size_t i = pos;
        while (i + 1 < len && !(in[i] == '\r' && in[i + 1] == '\n')) i++;
        if (i + 1 >= len) return 0;
        size_t size = 0;
        int digits = 0;
        for (size_t k = pos; k < i && in[k] != ';'; k++) {
            int d = hexval(in[k]);
            if (d < 0) return -1;
            size = size * 16 + (size_t)d;
            digits++;
            if (size > (1u << 30)) return -1;
        }
        if (!digits) return -1;
        pos = i + 2;
        if (size == 0) {
            if (len - pos >= 2 && in[pos] == '\r' && in[pos + 1] == '\n') { *out_len = o; return 1; }
            for (size_t t = pos; t + 3 < len; t++)
                if (in[t] == '\r' && in[t + 1] == '\n' && in[t + 2] == '\r' && in[t + 3] == '\n') { *out_len = o; return 1; }
            return 0;
        }
        if (len - pos < size + 2) return 0;
        if (o + size > out_cap) return -1;
        memcpy(out + o, in + pos, size);
        o += size;
        pos += size;
        if (in[pos] != '\r' || in[pos + 1] != '\n') return -1;
        pos += 2;
    }
}

/* ============================================================== server */

/* Reads until the end of the header block. Returns bytes in buf, or <0 (-1 closed/error, -431 too large). */
static long read_head(conn_t *c, uint8_t *buf, size_t cap, long *hdr_end)
{
    size_t total = 0;
    uint64_t deadline = plat_time_ms() + HEADER_DEADLINE_MS;
    for (;;) {
        long e = find_header_end(buf, total);
        if (e >= 0) { *hdr_end = e; return (long)total; }
        if (total >= cap) return -431;
        int r = conn_read(c, buf + total, cap - total);
        if (r > 0) { total += (size_t)r; continue; }
        if (r == LS_IO_TIMEOUT && plat_time_ms() < deadline) continue;
        return -1;
    }
}

static void url_split(const char *target, http_req *req)
{
    const char *q = strchr(target, '?');
    size_t plen = q ? (size_t)(q - target) : strlen(target);
    if (plen >= sizeof req->path) plen = sizeof req->path - 1;
    memcpy(req->path, target, plen);
    req->path[plen] = 0;
    if (q) ls_strlcpy(req->query, q + 1, sizeof req->query);
    else req->query[0] = 0;
}

bool http_req_header(const http_req *r, const char *name, char *out, size_t cap)
{
    return r->headers && hdr_get(r->headers, name, out, cap);
}

bool http_query_get(const http_req *r, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = r->query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && !strncmp(p, key, klen) && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = seg - klen - 1, n = 0;
            for (size_t i = 0; i < vlen && n + 1 < cap; i++) {
                if (v[i] == '%' && i + 2 < vlen && hexval(v[i + 1]) >= 0 && hexval(v[i + 2]) >= 0) {
                    out[n++] = (char)(hexval(v[i + 1]) * 16 + hexval(v[i + 2]));
                    i += 2;
                } else if (v[i] == '+') {
                    out[n++] = ' ';
                } else {
                    out[n++] = v[i];
                }
            }
            out[n] = 0;
            return true;
        }
        p = amp ? amp + 1 : p + seg;
    }
    return false;
}

/* Reads raw body bytes (first what arrived together with the headers, then from the connection). */
static int raw_read(http_req *r, void *buf, size_t cap)
{
    if (r->pre_pos < r->pre_len) {
        size_t k = r->pre_len - r->pre_pos;
        if (k > cap) k = cap;
        memcpy(buf, r->pre + r->pre_pos, k);
        r->pre_pos += k;
        return (int)k;
    }
    uint64_t deadline = plat_time_ms() + BODY_DEADLINE_MS;
    for (;;) {
        int got = conn_read(r->conn, buf, cap);
        if (got > 0) return got;
        if (got == LS_IO_TIMEOUT && plat_time_ms() < deadline && !g_quit && !(r->cancel && *r->cancel)) continue;
        return -1;                                      /* closed early, error, stalled or cancelled */
    }
}

enum { CS_SIZE = 0, CS_DATA, CS_CRLF, CS_DONE };

/* Reads one line (without CRLF) of chunk framing, one byte at a time. */
static bool raw_line(http_req *r, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        char c;
        if (raw_read(r, &c, 1) != 1) return false;
        if (c == '\n') { if (n && out[n - 1] == '\r') n--; out[n] = 0; return true; }
        if (n + 1 >= cap) return false;
        out[n++] = c;
    }
}

int http_stream_read(http_req *r, void *buf, size_t cap)
{
    if (cap == 0) return -1;
    if (!r->chunked) {
        if (r->remaining <= 0) return 0;
        size_t want = (int64_t)cap > r->remaining ? (size_t)r->remaining : cap;
        int got = raw_read(r, buf, want);
        if (got <= 0) return -1;
        r->remaining -= got;
        return got;
    }
    for (;;) {
        char line[80];
        switch (r->chunk_state) {
        case CS_DONE:
            return 0;
        case CS_DATA: {
            size_t want = (int64_t)cap > r->chunk_left ? (size_t)r->chunk_left : cap;
            int got = raw_read(r, buf, want);
            if (got <= 0) return -1;
            r->chunk_left -= got;
            if (r->chunk_left == 0) r->chunk_state = CS_CRLF;
            return got;
        }
        case CS_CRLF:
            if (!raw_line(r, line, sizeof line) || line[0]) return -1;
            r->chunk_state = CS_SIZE;
            break;
        default: {                                       /* CS_SIZE */
            if (!raw_line(r, line, sizeof line)) return -1;
            char *end;
            long long sz = strtoll(line, &end, 16);
            if (end == line || sz < 0) return -1;
            if (sz == 0) {
                while (raw_line(r, line, sizeof line) && line[0]) {}   /* optional trailers */
                r->chunk_state = CS_DONE;
                return 0;
            }
            r->chunk_left = sz;
            r->chunk_state = CS_DATA;
            break;
        }
        }
    }
}

void http_drain(http_req *r, size_t max_bytes)
{
    uint8_t tmp[2048];
    size_t total = 0;
    while (total < max_bytes) {
        int n = http_stream_read(r, tmp, sizeof tmp);
        if (n <= 0) break;
        total += (size_t)n;
    }
}

void http_send(http_res *res, int status, const char *content_type, const void *body, size_t len)
{
    if (res->sent) return;
    res->sent = true;
    char head[320];
    int n;
    if (status == 204 || len == 0) {
        n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nServer: VitaSend/" LS_APP_VERSION "\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n", status, status_text(status));
        len = 0;
    } else {
        n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nServer: VitaSend/" LS_APP_VERSION "\r\nContent-Type: %s\r\n"
                     "Content-Length: %u\r\nConnection: close\r\n\r\n",
                     status, status_text(status), content_type ? content_type : "application/octet-stream",
                     (unsigned)len);
    }
    if (len && len <= 8192) {                           /* one TLS record for small replies */
        uint8_t out[8192 + 320];
        memcpy(out, head, (size_t)n);
        memcpy(out + n, body, len);
        conn_write_all(res->conn, out, (size_t)n + len);
    } else {
        if (conn_write_all(res->conn, head, (size_t)n) == 0 && len)
            conn_write_all(res->conn, body, len);
    }
}

void http_send_json(http_res *res, int status, const char *json)
{
    http_send(res, status, "application/json", json, strlen(json));
}

void http_send_error(http_res *res, int status, const char *message)
{
    char buf[200];
    /* messages are fixed ASCII strings chosen by us, so no escaping is needed */
    int n = snprintf(buf, sizeof buf, "{\"message\":\"%s\"}", message);
    http_send(res, status, "application/json", buf, (size_t)n);
}

/* Reads the request body into req->body (Content-Length or chunked). Returns 0 or an HTTP status to answer with. */
static int read_body(http_req *req, bool chunked, uint8_t *head_buf, size_t pre_off, size_t pre_len)
{
    uint64_t deadline = plat_time_ms() + BODY_DEADLINE_MS;

    if (chunked) {
        size_t cap = HTTP_MAX_BODY + 4096, have = 0;
        uint8_t *raw = malloc(cap);
        uint8_t *dec = malloc(HTTP_MAX_BODY + 1);
        if (!raw || !dec) { free(raw); free(dec); return 500; }
        memcpy(raw, head_buf + pre_off, pre_len);
        have = pre_len;
        for (;;) {
            size_t out_len = 0;
            int rc = chunked_decode(raw, have, dec, HTTP_MAX_BODY, &out_len);
            if (rc == 1) { dec[out_len] = 0; req->body = dec; req->body_len = out_len; req->content_length = (int64_t)out_len; free(raw); return 0; }
            if (rc < 0 || have >= cap) { free(raw); free(dec); return rc < 0 ? 400 : 413; }
            int r = conn_read(req->conn, raw + have, cap - have);
            if (r > 0) { have += (size_t)r; continue; }
            if (r == LS_IO_TIMEOUT && plat_time_ms() < deadline) continue;
            free(raw); free(dec);
            return 408;
        }
    }

    if (req->content_length <= 0) return 0;
    if (req->content_length > HTTP_MAX_BODY) return 413;      /* (streaming routes never get here) */
    size_t need = (size_t)req->content_length;
    uint8_t *b = malloc(need + 1);
    if (!b) return 500;
    size_t have = pre_len > need ? need : pre_len;
    memcpy(b, head_buf + pre_off, have);
    while (have < need) {
        int r = conn_read(req->conn, b + have, need - have);
        if (r > 0) { have += (size_t)r; continue; }
        if (r == LS_IO_TIMEOUT && plat_time_ms() < deadline) continue;
        free(b);
        return 408;
    }
    b[need] = 0;
    req->body = b;
    req->body_len = need;
    return 0;
}

static void serve_connection(conn_t *c, const char *peer_ip)
{
    http_res res = { c, false };
    uint8_t *buf = malloc(HTTP_MAX_HEADER + 1);
    if (!buf) return;

    long hdr_end = 0;
    long total = read_head(c, buf, HTTP_MAX_HEADER, &hdr_end);
    if (total < 0) {
        if (total == -431) http_send_error(&res, 431, "Header too large");
        free(buf);
        return;
    }
    buf[hdr_end] = 0;                                   /* terminate the header block */

    http_req req;
    memset(&req, 0, sizeof req);
    req.conn = c;
    ls_strlcpy(req.peer_ip, peer_ip, sizeof req.peer_ip);

    /* request line */
    char *line_end = strstr((char *)buf, "\r\n");
    size_t line_len = line_end ? (size_t)(line_end - (char *)buf) : (size_t)hdr_end;
    char target[800], version[16];
    char line[1000];
    if (line_len >= sizeof line) { http_send_error(&res, 414, "URI too long"); free(buf); return; }
    memcpy(line, buf, line_len);
    line[line_len] = 0;
    if (sscanf(line, "%7s %799s %15s", req.method, target, version) != 3 || strncmp(version, "HTTP/1.", 7)) {
        http_send_error(&res, 400, "Bad request");
        free(buf);
        return;
    }
    if (strlen(target) >= sizeof req.path + sizeof req.query - 2) { http_send_error(&res, 414, "URI too long"); free(buf); return; }
    url_split(target, &req);
    req.headers = line_end ? line_end + 2 : (char *)buf + hdr_end;

    /* body framing */
    char v[64];
    bool chunked = false;
    if (hdr_get(req.headers, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked")) chunked = true;
    if (hdr_get(req.headers, "Content-Length", v, sizeof v)) {
        char *endp;
        long long cl = strtoll(v, &endp, 10);
        if (endp == v || cl < 0) { http_send_error(&res, 400, "Bad Content-Length"); free(buf); return; }
        req.content_length = cl;
    }
    if (hdr_get(req.headers, "Expect", v, sizeof v) && !ls_strncasecmp(v, "100-continue", 12)) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        conn_write_all(c, cont, sizeof cont - 1);
    }
    if (req.content_length > HTTP_MAX_BODY && !chunked && !api_wants_stream(req.method, req.path)) {
        http_send_error(&res, 413, "Body too large");
        free(buf);
        return;
    }

    size_t pre_off = (size_t)hdr_end + 4;
    size_t pre_len = (size_t)total - pre_off;
    int st;
    if (api_wants_stream(req.method, req.path)) {
        /* file upload: the handler pulls the body with http_stream_read() */
        req.pre = buf + pre_off;
        req.pre_len = pre_len;
        req.pre_pos = 0;
        req.chunked = chunked;
        req.chunk_state = CS_SIZE;
        req.remaining = chunked ? 0 : req.content_length;
        st = 0;
    } else {
        st = read_body(&req, chunked, buf, pre_off, pre_len);
    }
    if (st) {
        http_send_error(&res, st, st == 408 ? "Timeout" : st == 413 ? "Body too large" : st == 400 ? "Bad body" : "Error");
        free(req.body);
        free(buf);
        return;
    }

    api_handle(&req, &res);
    if (!res.sent) http_send_error(&res, 500, "No response");

    free(req.body);
    free(buf);
}

typedef struct {
    ls_sock     fd;
    ls_endpoint peer;
} conn_job;

static bool        s_use_tls;
static plat_mutex *s_conn_lock;
static int         s_conn_active;

static void conn_thread(void *arg)
{
    conn_job *j = arg;
    conn_t *c = conn_accept(j->fd, s_use_tls, IDLE_TIMEOUT_MS);   /* closes the socket on failure */
    if (c) {
        serve_connection(c, j->peer.ip);
        conn_close(c);
    }
    free(j);
    plat_mutex_lock(s_conn_lock);
    s_conn_active--;
    plat_mutex_unlock(s_conn_lock);
}

static void accept_thread(void *arg)
{
    ls_sock listener = (ls_sock)(intptr_t)arg;
    while (!g_quit) {
        ls_endpoint peer;
        ls_sock s = plat_tcp_accept(listener, &peer);
        if (s == LS_IO_TIMEOUT) { plat_sleep_ms(10); continue; }
        if (s < 0) { plat_sleep_ms(100); continue; }

        plat_mutex_lock(s_conn_lock);
        bool full = s_conn_active >= HTTP_MAX_CONN;
        if (!full) s_conn_active++;
        plat_mutex_unlock(s_conn_lock);
        if (full) { plat_sock_close(s); continue; }

        conn_job *j = malloc(sizeof *j);
        if (j) {
            j->fd = s;                                  /* fill in BEFORE the thread can run */
            j->peer = peer;
        }
        if (!j || !ls_spawn("ls_conn", conn_thread, j, CONN_STACK_BYTES)) {
            free(j);
            plat_sock_close(s);
            plat_mutex_lock(s_conn_lock);
            s_conn_active--;
            plat_mutex_unlock(s_conn_lock);
        }
    }
    plat_sock_close(listener);
}

bool http_server_start(uint16_t *port, bool use_tls)
{
    s_use_tls = use_tls;
    if (!s_conn_lock) s_conn_lock = plat_mutex_create();
    ls_sock l = plat_tcp_listen(*port, 16);
    if (l < 0) return false;
    uint16_t actual = plat_sock_local_port(l);
    if (actual) *port = actual;
    else if (*port == 0) { plat_sock_close(l); return false; }
    if (!ls_spawn("ls_accept", accept_thread, (void *)(intptr_t)l, 64 * 1024)) {
        ls_log("could not start the thread that accepts connections");
        plat_sock_close(l);
        return false;
    }
    return true;
}

/* ============================================================== client */

#define MAX_RESP (512 * 1024)
#define SEND_CHUNK 16384

void http_resp_free(http_resp *r)
{
    free(r->body);
    memset(r, 0, sizeof *r);
}

const char *http_err_text(int err)
{
    switch (err) {
    case HTTP_ERR_CONNECT:  return "could not connect";
    case HTTP_ERR_SEND:     return "connection lost while sending";
    case HTTP_ERR_REPLY:    return "no answer from the other device";
    case HTTP_ERR_ABORT:    return "cancelled";
    case HTTP_ERR_PROTOCOL: return "unexpected answer";
    default:                return "ok";
    }
}

void http_url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 4 < cap; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[n++] = (char)*p;
        } else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = 0;
}

typedef struct { const uint8_t *p; size_t left; } mem_src;

static int mem_read(void *ctx, void *buf, size_t cap)
{
    mem_src *m = ctx;
    size_t k = m->left < cap ? m->left : cap;
    memcpy(buf, m->p, k);
    m->p += k;
    m->left -= k;
    return (int)k;
}

bool http_request_ex(const char *ip, uint16_t port, bool https, const char *method, const char *path_and_query,
                     const char *content_type, const http_body_src *src, const http_opts *opts, http_resp *out)
{
    memset(out, 0, sizeof *out);
    out->err = HTTP_ERR_CONNECT;
    conn_t *c = conn_connect(ip, port, https, opts->connect_ms);
    if (!c) return false;

    char head[1024];
    int n = snprintf(head, sizeof head,
                     "%s %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: VitaSend/" LS_APP_VERSION "\r\n"
                     "Accept: */*\r\nConnection: close\r\n",
                     method, path_and_query, ip, (unsigned)port);
    if (src || content_type)
        n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: %s\r\nContent-Length: %lld\r\n",
                      content_type ? content_type : "application/json", (long long)(src ? src->len : 0));
    if (n > 0 && (size_t)n < sizeof head) n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    if (n <= 0 || (size_t)n >= sizeof head) { conn_close(c); out->err = HTTP_ERR_PROTOCOL; return false; }

    out->err = HTTP_ERR_SEND;
    conn_set_timeout(c, opts->send_ms);
    if (conn_write_all(c, head, (size_t)n) != 0) { conn_close(c); return false; }
    if (src && src->len > 0) {
        uint8_t *chunk = malloc(SEND_CHUNK);
        if (!chunk) { conn_close(c); return false; }
        int64_t sent = 0;
        while (sent < src->len) {
            size_t want = (src->len - sent) > SEND_CHUNK ? SEND_CHUNK : (size_t)(src->len - sent);
            int got = src->read(src->ctx, chunk, want);
            if (got < 0) { out->err = HTTP_ERR_ABORT; free(chunk); conn_close(c); return false; }
            if (got == 0) { free(chunk); conn_close(c); return false; }          /* source ended early */
            if ((opts->abort && *opts->abort) || g_quit) { out->err = HTTP_ERR_ABORT; free(chunk); conn_close(c); return false; }
            if (conn_write_all(c, chunk, (size_t)got) != 0) { free(chunk); conn_close(c); return false; }
            sent += got;
        }
        free(chunk);
    }

    /* ---- the answer.  Poll in short steps so an abort request is noticed quickly. */
    out->err = HTTP_ERR_REPLY;
    conn_set_timeout(c, 400);
    size_t   cap = 4096, have = 0;
    uint8_t *buf = malloc(cap);
    long     hdr_end = -1;
    int      status = 0;
    bool     chunked = false, complete = false;
    int64_t  clen = -1;
    uint8_t *dec = NULL;
    size_t   dec_len = 0;
    uint64_t deadline = plat_time_ms() + (uint64_t)opts->reply_ms;

    while (buf && !complete) {
        if (have + 1024 > cap) {
            if (cap >= MAX_RESP) break;
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        int r = conn_read(c, buf + have, cap - have);
        if (r > 0) {
            have += (size_t)r;
        } else if (r == LS_IO_TIMEOUT) {
            if (opts->abort && *opts->abort) { out->err = HTTP_ERR_ABORT; break; }
            if (!g_quit && plat_time_ms() < deadline) continue;
            break;
        } else {
            break;                                      /* EOF or error: evaluate what we have */
        }

        if (hdr_end < 0) {
            hdr_end = find_header_end(buf, have);
            if (hdr_end >= 0) {
                buf[hdr_end] = 0;
                if (sscanf((char *)buf, "HTTP/%*d.%*d %d", &status) != 1) break;
                const char *hs = strstr((char *)buf, "\r\n");
                char v[64];
                if (hs) {
                    if (hdr_get(hs + 2, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked")) chunked = true;
                    if (hdr_get(hs + 2, "Content-Length", v, sizeof v)) clen = strtoll(v, NULL, 10);
                }
                buf[hdr_end] = '\r';                    /* restore for later searches */
            }
        }
        if (hdr_end >= 0) {
            size_t off = (size_t)hdr_end + 4;
            if (status == 204 || status == 304 || (status >= 100 && status < 200)) {
                complete = true;
            } else if (chunked) {
                free(dec);
                dec = malloc(have - off + 1);
                if (!dec) break;
                int rc = chunked_decode(buf + off, have - off, dec, have - off, &dec_len);
                if (rc == 1) complete = true;
                else if (rc < 0) break;
            } else if (clen >= 0 && (int64_t)(have - off) >= clen) {
                complete = true;
            }
        }
    }

    bool ok = false;
    if (buf && hdr_end >= 0 && status > 0) {
        size_t off = (size_t)hdr_end + 4;
        if (status == 204 || status == 304) {
            out->body = calloc(1, 1);
            ok = out->body != NULL;
        } else if (chunked) {
            if (complete) {
                out->body = (char *)dec;
                out->body_len = dec_len;
                out->body[dec_len] = 0;
                dec = NULL;
                ok = true;
            }
        } else {
            size_t avail = have - off;
            size_t take = clen >= 0 ? (size_t)clen : avail;
            if (take <= avail && take <= MAX_RESP) {
                out->body = malloc(take + 1);
                if (out->body) {
                    memcpy(out->body, buf + off, take);
                    out->body[take] = 0;
                    out->body_len = take;
                    ok = true;
                }
            }
        }
        out->status = status;
    }
    if (ok) {
        out->err = HTTP_ERR_NONE;
        conn_peer_fingerprint(c, out->peer_fp);
    } else if (out->err == HTTP_ERR_REPLY && hdr_end >= 0) {
        out->err = HTTP_ERR_PROTOCOL;
    }
    free(dec);
    free(buf);
    conn_close(c);
    if (!ok) {
        int e = out->err;
        http_resp_free(out);
        out->err = e;
    }
    return ok;
}

bool http_request(const char *ip, uint16_t port, bool https, const char *method, const char *path_and_query,
                  const char *content_type, const void *body, size_t body_len, int timeout_ms, http_resp *out)
{
    mem_src m = { body, body_len };
    http_body_src src = { mem_read, &m, (int64_t)body_len };
    http_opts o = { timeout_ms, timeout_ms, timeout_ms * 2, NULL };
    return http_request_ex(ip, port, https, method, path_and_query, content_type,
                           (body_len || content_type) ? &src : NULL, &o, out);
}
