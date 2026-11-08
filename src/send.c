/*
 * send.c - Sending files and text: collects files and folders, asks the other device with prepare-upload (PIN, 401/403/409/429),
 * streams one upload per accepted file, cancel, and Retry with the files that did not arrive.
 *
 * Author: trashman (VitaSend)
 */
#include "send.h"
#include "history.h"
#include "http.h"
#include "identity.h"
#include "json.h"
#include "plat.h"
#include "util.h"
#include "xfer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PREPARE_REPLY_MS  (5 * 60 * 1000)     /* the person on the other side may take a while */
#define UPLOAD_REPLY_MS   60000
#define STALL_MS          20000
#define CONNECT_MS        6000
#define SEND_STACK_BYTES  (256 * 1024)
#define MAX_DEPTH         10

/* ============================================================== collecting files */

static bool add_item(send_item **list, int *n, int *cap, const char *path, const char *name, uint64_t size)
{
    if (*n >= SEND_MAX_FILES) return false;
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        send_item *nl = realloc(*list, (size_t)nc * sizeof **list);
        if (!nl) return false;
        *list = nl;
        *cap = nc;
    }
    send_item *it = &(*list)[(*n)++];
    memset(it, 0, sizeof *it);
    ls_strlcpy(it->path, path, sizeof it->path);
    ls_strlcpy(it->name, name, sizeof it->name);
    it->size = size;
    return true;
}

static void collect_dir(const char *dir, const char *prefix, send_item **list, int *n, int *cap, int depth)
{
    plat_dirent *v = NULL;
    int cnt = plat_list_dir(dir, &v);
    if (cnt < 0) return;
    for (int i = 0; i < cnt && *n < SEND_MAX_FILES; i++) {
        char full[600], rel[600];
        snprintf(full, sizeof full, "%s/%s", dir, v[i].name);
        snprintf(rel, sizeof rel, "%s/%s", prefix, v[i].name);
        if (v[i].is_dir) {
            if (depth < MAX_DEPTH) collect_dir(full, rel, list, n, cap, depth + 1);
        } else {
            add_item(list, n, cap, full, rel, v[i].size);
        }
    }
    free(v);
}

bool send_collect(const char *path, send_item **list, int *n, int *cap)
{
    bool is_dir = false;
    uint64_t size = 0;
    if (!plat_path_info(path, &is_dir, &size)) return false;
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *colon = strchr(base, ':');
    if (colon) base = colon + 1;                                      /* "ux0:" style roots */
    if (!*base) return false;
    if (is_dir) collect_dir(path, base, list, n, cap, 1);
    else add_item(list, n, cap, path, base, size);
    return true;
}

/* ============================================================== job */

typedef struct {
    ls_peer    peer;
    send_item *items;                 /* NULL for a text message */
    bool      *done;                  /* per item: delivered (or deliberately left out by the receiver) */
    int        n;
    char      *text;                  /* malloc'd, text message only */
    int        outcome;               /* 0 = never ran, 1 = did not end well (job_free() remembers what is left), 2 = fine */
} send_job;

/* ---- what a retry would send (the last job that did not end well) ---- */
static plat_mutex *s_retry_lock;
static struct {
    bool       valid;
    ls_peer    peer;
    send_item *items;
    int        n;
    char      *text;
} s_last;

static void retry_forget_locked(void)
{
    free(s_last.items);
    free(s_last.text);
    memset(&s_last, 0, sizeof s_last);
}

static void retry_remember(const send_job *job)
{
    if (!s_retry_lock) return;
    plat_mutex_lock(s_retry_lock);
    retry_forget_locked();
    if (job->text) {
        s_last.text = strdup(job->text);
        s_last.valid = s_last.text != NULL;
    } else {
        int left = 0;
        for (int i = 0; i < job->n; i++) if (!job->done[i]) left++;
        if (left > 0) {
            s_last.items = malloc((size_t)left * sizeof *s_last.items);
            if (s_last.items) {
                for (int i = 0; i < job->n; i++) if (!job->done[i]) s_last.items[s_last.n++] = job->items[i];
                s_last.valid = true;
            }
        }
    }
    if (s_last.valid) s_last.peer = job->peer;
    plat_mutex_unlock(s_retry_lock);
}

int send_retry_count(void)
{
    if (!s_retry_lock) return 0;
    plat_mutex_lock(s_retry_lock);
    int n = !s_last.valid ? 0 : s_last.text ? 1 : s_last.n;
    plat_mutex_unlock(s_retry_lock);
    return n;
}

static volatile bool s_need_pin;
static volatile bool s_pin_ready;
static char          s_pin[16];
static plat_mutex   *s_pin_lock;

bool send_needs_pin(void) { return s_need_pin; }

void send_give_pin(const char *pin)
{
    if (!s_pin_lock) s_pin_lock = plat_mutex_create();
    plat_mutex_lock(s_pin_lock);
    if (pin) ls_strlcpy(s_pin, pin, sizeof s_pin);
    else s_pin[0] = 0;
    s_pin_ready = true;
    plat_mutex_unlock(s_pin_lock);
}

static const char *mime_for(const char *name)
{
    static const struct { const char *ext, *mime; } t[] = {
        { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "png", "image/png" }, { "gif", "image/gif" },
        { "webp", "image/webp" }, { "bmp", "image/bmp" }, { "mp4", "video/mp4" }, { "mkv", "video/x-matroska" },
        { "avi", "video/x-msvideo" }, { "mov", "video/quicktime" }, { "mp3", "audio/mpeg" }, { "ogg", "audio/ogg" },
        { "wav", "audio/wav" }, { "flac", "audio/flac" }, { "m4a", "audio/mp4" }, { "pdf", "application/pdf" },
        { "txt", "text/plain" }, { "html", "text/html" }, { "json", "application/json" }, { "zip", "application/zip" },
        { "apk", "application/vnd.android.package-archive" },
    };
    const char *dot = strrchr(name, '.');
    if (dot) {
        for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
            if (!ls_strcasecmp(dot + 1, t[i].ext)) return t[i].mime;
    }
    return "application/octet-stream";
}

static void history_finish(const send_job *job, hist_status st, uint64_t bytes, int nfiles)
{
    hist_entry h;
    memset(&h, 0, sizeof h);
    h.time = (int64_t)time(NULL);
    h.outgoing = true;
    h.status = st;
    ls_strlcpy(h.peer, job->peer.alias, sizeof h.peer);
    h.nfiles = nfiles;
    h.bytes = bytes;
    if (job->text) {
        h.is_text = true;
        ls_strlcpy(h.text, job->text, sizeof h.text);
        ls_strlcpy(h.summary, job->text, sizeof h.summary);
    } else if (job->n > 0) {
        if (job->n > 1) snprintf(h.summary, sizeof h.summary, "%.60s (+%d more)", job->items[0].name, job->n - 1);
        else ls_strlcpy(h.summary, job->items[0].name, sizeof h.summary);
        ls_strlcpy(h.path, job->items[0].path, sizeof h.path);
    }
    history_add(&h);
}

static void job_free(send_job *job)
{
    if (job->outcome == 1) retry_remember(job);
    else if (job->outcome == 2 && s_retry_lock) { plat_mutex_lock(s_retry_lock); retry_forget_locked(); plat_mutex_unlock(s_retry_lock); }
    free(job->items);
    free(job->done);
    free(job->text);
    free(job);
}

/* ---- file body source for http_request_ex ---- */
typedef struct {
    FILE *f;
    bool  failed;
} file_src;

static int file_read(void *ctx, void *buf, size_t cap)
{
    file_src *fs = ctx;
    if (*xfer_cancel_flag() || g_quit) return -1;
    size_t k = fread(buf, 1, cap, fs->f);
    if (k == 0) { fs->failed = ferror(fs->f) != 0; return 0; }
    xfer_add_bytes(k);
    return (int)k;
}

typedef struct {
    const uint8_t *p;
    size_t         left;
} text_src;

static int text_read(void *ctx, void *buf, size_t cap)
{
    text_src *t = ctx;
    size_t k = t->left < cap ? t->left : cap;
    memcpy(buf, t->p, k);
    t->p += k;
    t->left -= k;
    xfer_add_bytes(k);
    return (int)k;
}

/* Explains an HTTP failure in words the user can act on. */
static void explain(char *out, size_t cap, const http_resp *r, bool ok, const char *what)
{
    if (!ok) {
        if (r->err == HTTP_ERR_CONNECT) snprintf(out, cap, "%s: %s (%s)", what, http_err_text(r->err), plat_net_error_text());
        else snprintf(out, cap, "%s: %s", what, http_err_text(r->err));
        return;
    }
    switch (r->status) {
    case 401: snprintf(out, cap, "%s: wrong or missing PIN", what); break;
    case 403: snprintf(out, cap, "%s: the other device said no", what); break;
    case 409: snprintf(out, cap, "%s: the other device is busy with another transfer", what); break;
    case 422: snprintf(out, cap, "%s: the file arrived damaged (checksum)", what); break;
    case 429: snprintf(out, cap, "%s: too many attempts, try again in a minute", what); break;
    default:  snprintf(out, cap, "%s: the other device answered HTTP %d", what, r->status); break;
    }
}

static void tell_cancel(const send_job *job, const char *sid)
{
    char path[160], enc[100];
    http_url_encode(sid, enc, sizeof enc);
    snprintf(path, sizeof path, "/api/localsend/v2/cancel?sessionId=%s", enc);
    http_resp r;
    http_request(job->peer.ip, job->peer.port, job->peer.https, "POST", path, NULL, NULL, 0, 3000, &r);
    http_resp_free(&r);
}

/* plain memory source (the JSON request) - not counted as transfer progress */
typedef struct {
    const uint8_t *p;
    size_t         left;
} mem_ctx;

static int mem_read(void *ctx, void *buf, size_t cap)
{
    mem_ctx *m = ctx;
    size_t k = m->left < cap ? m->left : cap;
    memcpy(buf, m->p, k);
    m->p += k;
    m->left -= k;
    return (int)k;
}

static bool wait_for_pin(char *pin, size_t cap)
{
    s_pin_ready = false;
    s_need_pin = true;
    uint64_t deadline = plat_time_ms() + 5 * 60 * 1000;
    while (!s_pin_ready && !*xfer_cancel_flag() && !g_quit && plat_time_ms() < deadline) plat_sleep_ms(50);
    s_need_pin = false;
    if (!s_pin_ready) return false;
    if (!s_pin_lock) s_pin_lock = plat_mutex_create();
    plat_mutex_lock(s_pin_lock);
    ls_strlcpy(pin, s_pin, cap);
    plat_mutex_unlock(s_pin_lock);
    return pin[0] != 0;
}

static void send_thread(void *arg)
{
    send_job *job = arg;
    const ls_peer *peer = &job->peer;
    job->outcome = 1;                        /* every successful way out below sets 2 */
    char msg[200];
    char sid[100] = "";
    json_val *reply = NULL;
    const json_val *tokens = NULL;
    uint64_t total = 0;
    int nfiles = job->text ? 1 : job->n;
    if (job->text) total = strlen(job->text);
    else for (int i = 0; i < job->n; i++) total += job->items[i].size;

    xfer_set_totals(nfiles, total);
    xfer_set_message("Asking %s...", peer->alias);

    /* ---------------- prepare-upload ---------------- */
    jw w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "info");
    identity_json(&w, true, 0);
    jw_key(&w, "files");
    jw_obj(&w);
    if (job->text) {
        jw_key(&w, "msg");
        jw_obj(&w);
        jw_kstr(&w, "id", "msg");
        jw_kstr(&w, "fileName", "message.txt");
        jw_kint(&w, "size", (int64_t)strlen(job->text));
        jw_kstr(&w, "fileType", "text/plain");
        jw_kstr(&w, "preview", job->text);
        jw_end_obj(&w);
    } else {
        for (int i = 0; i < job->n; i++) {
            char id[16];
            snprintf(id, sizeof id, "f%d", i);
            jw_key(&w, id);
            jw_obj(&w);
            jw_kstr(&w, "id", id);
            jw_kstr(&w, "fileName", job->items[i].name);
            jw_kint(&w, "size", (int64_t)job->items[i].size);
            jw_kstr(&w, "fileType", mime_for(job->items[i].name));
            jw_end_obj(&w);
        }
    }
    jw_end_obj(&w);
    jw_end_obj(&w);
    if (w.oom) {
        jw_free(&w);
        xfer_end(XF_FAILED, "Out of memory");
        history_finish(job, H_FAILED, 0, 0);
        job_free(job);
        return;
    }

    http_resp resp;
    memset(&resp, 0, sizeof resp);
    bool ok = false;
    char pin_enc[40] = "";
    for (int attempt = 0;; attempt++) {
        char path[128] = "/api/localsend/v2/prepare-upload";
        if (pin_enc[0]) {
            size_t l = strlen(path);
            snprintf(path + l, sizeof path - l, "?pin=%s", pin_enc);
        }
        mem_ctx mc = { (const uint8_t *)w.buf, w.len };
        http_body_src src = { mem_read, &mc, (int64_t)w.len };
        http_opts o = { CONNECT_MS, STALL_MS, PREPARE_REPLY_MS, xfer_cancel_flag() };
        ok = http_request_ex(peer->ip, peer->port, peer->https, "POST", path, "application/json", &src, &o, &resp);
        if (ok && resp.status == 401 && attempt < 3) {
            http_resp_free(&resp);
            xfer_set_message("%s needs a PIN", peer->alias);
            char pin[16];
            if (!wait_for_pin(pin, sizeof pin)) {
                jw_free(&w);
                if (*xfer_cancel_flag()) xfer_end(XF_CANCELLED, "Cancelled");
                else xfer_end(XF_FAILED, "No PIN entered");
                history_finish(job, H_FAILED, 0, 0);
                job_free(job);
                return;
            }
            http_url_encode(pin, pin_enc, sizeof pin_enc);
            xfer_set_message("Asking %s...", peer->alias);
            continue;
        }
        break;
    }
    jw_free(&w);

    if (ok && peer->https && resp.peer_fp[0] && peer->fingerprint[0] && ls_strcasecmp(resp.peer_fp, peer->fingerprint) != 0)
        ls_log("note: %s presented a different certificate than it announced", peer->alias);

    if (!ok || (resp.status != 200 && resp.status != 204)) {
        bool cancelled = !ok && resp.err == HTTP_ERR_ABORT;
        explain(msg, sizeof msg, &resp, ok, "Could not start");
        bool rejected = ok && resp.status == 403;
        http_resp_free(&resp);
        if (cancelled) xfer_end(XF_CANCELLED, "Cancelled");
        else xfer_end(XF_FAILED, "%s", msg);
        history_finish(job, cancelled ? H_CANCELLED : rejected ? H_REJECTED : H_FAILED, 0, 0);
        ls_log("sending to %s: %s", peer->alias, cancelled ? "cancelled" : msg);
        job_free(job);
        return;
    }

    if (resp.status == 204) {                      /* "nothing to transfer": a text message is already delivered */
        http_resp_free(&resp);
        xfer_add_bytes(total);
        job->outcome = 2;
        xfer_end(XF_DONE, job->text ? "Message delivered to %s" : "%s has everything already", peer->alias);
        history_finish(job, H_OK, total, nfiles);
        job_free(job);
        return;
    }

    reply = json_parse(resp.body, resp.body_len, msg, sizeof msg);
    http_resp_free(&resp);
    ls_strlcpy(sid, json_str(json_get(reply, "sessionId"), ""), sizeof sid);
    tokens = json_get(reply, "files");
    if (!reply || !sid[0] || !tokens || tokens->type != J_OBJ) {
        json_free(reply);
        xfer_end(XF_FAILED, "%s sent an answer we cannot understand", peer->alias);
        history_finish(job, H_FAILED, 0, 0);
        job_free(job);
        return;
    }

    /* ---------------- upload every accepted file ---------------- */
    char enc_sid[100];
    http_url_encode(sid, enc_sid, sizeof enc_sid);

    int sendable = 0;
    uint64_t sendable_bytes = 0;
    if (job->text) {
        sendable = 1;
        sendable_bytes = total;
    } else {
        for (int i = 0; i < job->n; i++) {
            char id[16];
            snprintf(id, sizeof id, "f%d", i);
            if (json_str(json_get(tokens, id), NULL)) { sendable++; sendable_bytes += job->items[i].size; }
        }
    }
    xfer_set_totals(sendable, sendable_bytes);
    xfer_set_state(XF_ACTIVE);

    bool failed = false, cancelled = false;
    int sent_files = 0;
    int count = job->text ? 1 : job->n;
    for (int i = 0; i < count && !failed && !cancelled; i++) {
        char id[16];
        const char *name;
        if (job->text) { ls_strlcpy(id, "msg", sizeof id); name = "message"; }
        else { snprintf(id, sizeof id, "f%d", i); name = job->items[i].name; }
        const char *token = json_str(json_get(tokens, id), NULL);
        if (!token) {                                            /* the receiver left this file out: not ours to resend */
            if (!job->text) job->done[i] = true;
            continue;
        }

        xfer_set_current(name);
        xfer_set_message("Sending to %s", peer->alias);

        char enc_id[40], enc_tok[100], path[400];
        http_url_encode(id, enc_id, sizeof enc_id);
        http_url_encode(token, enc_tok, sizeof enc_tok);
        snprintf(path, sizeof path, "/api/localsend/v2/upload?sessionId=%s&fileId=%s&token=%s", enc_sid, enc_id, enc_tok);

        http_opts o = { CONNECT_MS, STALL_MS, UPLOAD_REPLY_MS, xfer_cancel_flag() };
        http_resp ur;
        bool uok;
        if (job->text) {
            text_src ts = { (const uint8_t *)job->text, strlen(job->text) };
            http_body_src src = { text_read, &ts, (int64_t)ts.left };
            uok = http_request_ex(peer->ip, peer->port, peer->https, "POST", path, "application/octet-stream", &src, &o, &ur);
        } else {
            file_src fs = { fopen(job->items[i].path, "rb"), false };
            if (!fs.f) {
                snprintf(msg, sizeof msg, "Cannot read %.150s", name);
                failed = true;
                break;
            }
            http_body_src src = { file_read, &fs, (int64_t)job->items[i].size };
            uok = http_request_ex(peer->ip, peer->port, peer->https, "POST", path, "application/octet-stream", &src, &o, &ur);
            fclose(fs.f);
            if (!uok && fs.failed) snprintf(msg, sizeof msg, "Read error on %.150s", name);
        }
        if (uok && ur.status == 200) {
            xfer_file_done();
            sent_files++;
            if (!job->text) job->done[i] = true;
        } else if (!uok && ur.err == HTTP_ERR_ABORT) {
            cancelled = true;
        } else {
            char what[140];
            snprintf(what, sizeof what, "Failed on %.100s", name);
            explain(msg, sizeof msg, &ur, uok, what);
            failed = true;
        }
        http_resp_free(&ur);
    }
    json_free(reply);
    if (*xfer_cancel_flag() && !failed) cancelled = true;

    xfer_status xs;
    xfer_snapshot(&xs);
    if (cancelled) {
        tell_cancel(job, sid);
        xfer_end(XF_CANCELLED, "Cancelled");
        history_finish(job, H_CANCELLED, xs.bytes_done, sent_files);
    } else if (failed) {
        tell_cancel(job, sid);
        xfer_end(XF_FAILED, "%s", msg);
        history_finish(job, H_FAILED, xs.bytes_done, sent_files);
        ls_log("sending to %s failed: %s", peer->alias, msg);
    } else if (sendable == 0) {
        xfer_end(XF_CANCELLED, "%s did not accept any file", peer->alias);
        history_finish(job, H_REJECTED, 0, 0);
    } else {
        job->outcome = 2;
        if (job->text) xfer_end(XF_DONE, "Message delivered to %s", peer->alias);
        else xfer_end(XF_DONE, "Sent %d file%s to %s", sent_files, sent_files == 1 ? "" : "s", peer->alias);
        history_finish(job, H_OK, xs.bytes_done, sent_files);
        ls_log("sent %d file(s) to %s", sent_files, peer->alias);
    }
    job_free(job);
}

/* ============================================================== start */

static bool start_job(send_job *job)
{
    if (!s_retry_lock) s_retry_lock = plat_mutex_create();     /* first job starts on the UI thread, before any send thread */
    if (!xfer_begin(true, job->peer.alias)) {
        job_free(job);
        return false;
    }
    s_need_pin = false;
    if (!ls_spawn("ls_send", send_thread, job, SEND_STACK_BYTES)) {
        xfer_end(XF_FAILED, "Cannot start the sending thread");
        job_free(job);
        return false;
    }
    return true;
}

bool send_start_files(const ls_peer *peer, send_item *items, int n)
{
    send_job *job = calloc(1, sizeof *job);
    if (!job) { free(items); return false; }
    job->peer = *peer;
    job->items = items;
    job->n = n;
    job->done = calloc((size_t)(n > 0 ? n : 1), sizeof *job->done);
    if (!job->done) { free(items); free(job); return false; }
    return start_job(job);
}

bool send_start_text(const ls_peer *peer, const char *text)
{
    send_job *job = calloc(1, sizeof *job);
    if (!job) return false;
    job->peer = *peer;
    job->text = strdup(text);
    if (!job->text) { free(job); return false; }
    return start_job(job);
}

bool send_retry(void)
{
    if (!s_retry_lock || xfer_busy()) return false;
    send_job *job = calloc(1, sizeof *job);
    if (!job) return false;
    plat_mutex_lock(s_retry_lock);
    bool ok = s_last.valid;
    if (ok) {
        job->peer = s_last.peer;
        if (s_last.text) {
            job->text = strdup(s_last.text);
            ok = job->text != NULL;
        } else {
            job->items = malloc((size_t)s_last.n * sizeof *job->items);
            job->done = calloc((size_t)s_last.n, sizeof *job->done);
            ok = job->items && job->done;
            if (ok) { memcpy(job->items, s_last.items, (size_t)s_last.n * sizeof *job->items); job->n = s_last.n; }
        }
    }
    plat_mutex_unlock(s_retry_lock);
    if (!ok) { job_free(job); return false; }
    return start_job(job);
}
