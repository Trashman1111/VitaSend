/*
 * api.c - the LocalSend REST API routes (protocol v2.1).
 *
 *   Discovery:  GET  /api/localsend/v2/info
 *               POST /api/localsend/v2/register
 *   Receiving:  POST /api/localsend/v2/prepare-upload   (rx.c)
 *               POST /api/localsend/v2/upload           (rx.c, body is streamed to disk)
 *               POST /api/localsend/v2/cancel           (rx.c)
 *   Not supported: the browser "download" API (prepare-download / download) -> 403.
 *
 * Author: trashman (VitaSend)
 */
#include "http.h"
#include "identity.h"
#include "json.h"
#include "peers.h"
#include "rx.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define API "/api/localsend/v2/"

bool api_wants_stream(const char *method, const char *path)
{
    return !strcmp(method, "POST") && !strcmp(path, API "upload");
}

static void reply_identity(http_res *res, bool with_net)
{
    jw w;
    jw_init(&w);
    identity_json(&w, with_net, 0);
    if (w.oom) http_send_error(res, 500, "Out of memory");
    else http_send_json(res, 200, w.buf);
    jw_free(&w);
}

/* GET /info  - debugging / legacy discovery. ?fingerprint=<theirs> lets a device detect that it found itself. */
static void handle_info(http_req *req, http_res *res)
{
    char fp[80];
    if (http_query_get(req, "fingerprint", fp, sizeof fp) && !strcmp(fp, g_id.fingerprint)) {
        http_send_error(res, 412, "Self-discovered");
        return;
    }
    reply_identity(res, false);
}

/* POST /register - "I am here, and this is who I am". We answer with who we are. */
static void handle_register(http_req *req, http_res *res)
{
    if (!req->body || req->body_len == 0) {
        http_send_error(res, 400, "Invalid body");
        return;
    }
    char err[80];
    json_val *j = json_parse((const char *)req->body, req->body_len, err, sizeof err);
    if (!j || j->type != J_OBJ) {
        ls_log("register from %s: bad JSON (%s)", req->peer_ip, err);
        json_free(j);
        http_send_error(res, 400, "Invalid body");
        return;
    }

    const char *alias = json_str(json_get(j, "alias"), NULL);
    const char *fp    = json_str(json_get(j, "fingerprint"), NULL);
    if (!alias || !*alias || !fp || !*fp || strlen(fp) >= sizeof(((ls_peer *)0)->fingerprint)) {
        json_free(j);
        http_send_error(res, 400, "Invalid body");
        return;
    }
    if (!strcmp(fp, g_id.fingerprint)) {
        json_free(j);
        http_send_error(res, 412, "Self-discovered");
        return;
    }

    ls_peer p;
    memset(&p, 0, sizeof p);
    ls_strlcpy(p.alias, alias, sizeof p.alias);
    ls_strlcpy(p.fingerprint, fp, sizeof p.fingerprint);
    ls_strlcpy(p.model, json_str(json_get(j, "deviceModel"), ""), sizeof p.model);
    ls_strlcpy(p.type, json_str(json_get(j, "deviceType"), "desktop"), sizeof p.type);
    ls_strlcpy(p.ip, req->peer_ip, sizeof p.ip);
    double port = json_num(json_get(j, "port"), LS_DEFAULT_PORT);
    p.port = (port >= 1 && port <= 65535) ? (uint16_t)port : LS_DEFAULT_PORT;
    p.https = strcmp(json_str(json_get(j, "protocol"), "https"), "http") != 0;
    p.download = json_bool(json_get(j, "download"), false);
    json_free(j);

    bool is_new = peers_upsert(&p);
    ls_log("%s %s (%s) %s:%u via /register", is_new ? "found" : "refreshed", p.alias, p.type, p.ip, p.port);
    reply_identity(res, true);
}

void api_handle(http_req *req, http_res *res)
{
    if (strncmp(req->path, API, strlen(API)) != 0) {
        http_send_error(res, 404, "Not found");
        return;
    }
    const char *route = req->path + strlen(API);
    bool is_get = !strcmp(req->method, "GET");
    bool is_post = !strcmp(req->method, "POST");

    if (!strcmp(route, "info")) {
        if (!is_get) { http_send_error(res, 405, "Method not allowed"); return; }
        handle_info(req, res);
    } else if (!strcmp(route, "register")) {
        if (!is_post) { http_send_error(res, 405, "Method not allowed"); return; }
        handle_register(req, res);
    } else if (!strcmp(route, "prepare-upload")) {
        if (!is_post) { http_send_error(res, 405, "Method not allowed"); return; }
        rx_handle_prepare(req, res);
    } else if (!strcmp(route, "upload")) {
        if (!is_post) { http_send_error(res, 405, "Method not allowed"); return; }
        rx_handle_upload(req, res);
    } else if (!strcmp(route, "cancel")) {
        if (!is_post) { http_send_error(res, 405, "Method not allowed"); return; }
        rx_handle_cancel(req, res);
    } else if (!strcmp(route, "prepare-download") || !strcmp(route, "download")) {
        /* the browser download API is not supported */
        http_send_error(res, 403, "Rejected");
    } else {
        http_send_error(res, 404, "Not found");
    }
}
