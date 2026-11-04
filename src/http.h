/*
 * http.h - a small HTTP/1.1 server and client on top of conn_t (so HTTPS comes for free).
 *
 * Server: one thread per connection, "Connection: close" after every response.
 * Client: one request per connection.
 * Both understand Content-Length and chunked bodies.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_HTTP_H
#define LS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tls.h"

#define HTTP_MAX_HEADER  16384
#define HTTP_MAX_BODY    (256 * 1024)     /* bigger bodies are only accepted by streaming routes */
#define HTTP_MAX_CONN    6

typedef struct http_req {
    conn_t  *conn;
    char     method[8];
    char     path[256];
    char     query[512];
    char     peer_ip[16];
    char    *headers;                     /* raw "Name: value\r\n..." block (NUL terminated) */
    int64_t  content_length;              /* 0 when there is no body                         */
    uint8_t *body;                        /* NUL terminated copy; NULL if streaming/absent   */
    size_t   body_len;
    /* streaming state (file uploads): see api_wants_stream() */
    uint8_t *pre;                         /* body bytes that arrived together with the headers */
    size_t   pre_len, pre_pos;
    int64_t  remaining;                   /* Content-Length bodies: bytes not yet consumed     */
    bool     chunked;                     /* Transfer-Encoding: chunked body                   */
    int      chunk_state;
    int64_t  chunk_left;
    volatile bool *cancel;                /* handler may point this at a flag that aborts a stalled read */
} http_req;

typedef struct http_res {
    conn_t *conn;
    bool    sent;
} http_res;

/* ---- server ---- */
/* Opens the listening port: *port = the port to try (0 = any free one). On success *port is the port really in use. */
bool http_server_start(uint16_t *port, bool use_tls);

/* ---- helpers for handlers ---- */
bool http_req_header(const http_req *r, const char *name, char *out, size_t cap);
bool http_query_get(const http_req *r, const char *key, char *out, size_t cap);   /* url-decoded */
int  http_stream_read(http_req *r, void *buf, size_t cap);   /* >0, 0 = body finished, <0 = error */
void http_drain(http_req *r, size_t max_bytes);              /* swallow the rest of an unwanted body (so the sender sees our answer) */

void http_send(http_res *res, int status, const char *content_type, const void *body, size_t len);
void http_send_json(http_res *res, int status, const char *json);
void http_send_error(http_res *res, int status, const char *message);   /* {"message": "..."} */

/* ---- client ---- */
enum { HTTP_ERR_NONE = 0, HTTP_ERR_CONNECT, HTTP_ERR_SEND, HTTP_ERR_REPLY, HTTP_ERR_ABORT, HTTP_ERR_PROTOCOL };

typedef struct {
    int      status;
    int      err;                         /* HTTP_ERR_*; only meaningful when the call returned false */
    char    *body;                        /* NUL terminated, free with http_resp_free */
    size_t   body_len;
    char     peer_fp[65];                 /* fingerprint of the server certificate (HTTPS) */
} http_resp;

/* Streaming request body: read() returns >0 bytes, 0 at the end, <0 to abort the request. */
typedef struct {
    int     (*read)(void *ctx, void *buf, size_t cap);
    void     *ctx;
    int64_t   len;                        /* becomes Content-Length */
} http_body_src;

typedef struct {
    int            connect_ms;            /* TCP connect + TLS handshake */
    int            send_ms;               /* a send that makes no progress for this long fails */
    int            reply_ms;              /* how long to wait for the answer once the request is sent */
    volatile bool *abort;                 /* optional: set to true from another thread to give up quickly */
} http_opts;

bool http_request_ex(const char *ip, uint16_t port, bool https, const char *method, const char *path_and_query,
                     const char *content_type, const http_body_src *src, const http_opts *opts, http_resp *out);
bool http_request(const char *ip, uint16_t port, bool https, const char *method, const char *path_and_query,
                  const char *content_type, const void *body, size_t body_len, int timeout_ms, http_resp *out);
void http_resp_free(http_resp *r);
const char *http_err_text(int err);
void http_url_encode(const char *in, char *out, size_t cap);

/* implemented in api.c */
void api_handle(http_req *req, http_res *res);
bool api_wants_stream(const char *method, const char *path);

#endif
