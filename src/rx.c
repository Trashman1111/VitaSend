/*
 * rx.c - Receiving files: POST /prepare-upload (asks the user, or accepts by the Quick Save setting; checks PIN, free space, names),
 * POST /upload (streams the body to <name>.part and renames it when complete; SHA-256 check) and POST /cancel.
 *
 * Author: trashman (VitaSend)
 */
#include "rx.h"
#include "vpk.h"
#include "identity.h"
#include "json.h"
#include "peers.h"
#include "plat.h"
#include "settings.h"
#include "tls.h"
#include "ui.h"
#include "util.h"
#include "xfer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ASK_TIMEOUT_MS      90000
#define IDLE_TIMEOUT_MS     120000     /* nothing happening in an accepted session */
#define IDLE_AFTER_FAIL_MS  15000
#define WRITE_BUF           (128 * 1024)
#define READ_CHUNK          16384
#define PATH_MAX_LEN        520

typedef enum { RF_PENDING, RF_ACTIVE, RF_DONE, RF_FAILED } rf_state;

typedef struct {
    char     id[72];
    char     token[33];
    char     name[200];                /* sanitised relative name (what the user sees) */
    char     path[PATH_MAX_LEN];       /* final location */
    char     sha[65];                  /* expected SHA-256, "" if the sender did not say */
    uint64_t size;
    rf_state st;
} rx_file;

static plat_mutex *s_lock;

/* ---- the question we are asking the user ---- */
enum { Q_NONE, Q_ASKING, Q_ACCEPT, Q_REJECT };
static int        s_q;
static rx_request s_qinfo;

/* ---- the accepted session ---- */
static struct {
    bool     alive;                    /* still accepting uploads */
    int      refs;                     /* uploads in flight (they use files[]) */
    char     sid[33];
    char     ip[16];
    char     alias[64];
    rx_file *files;
    int      nfiles, ndone, nfailed, nskipped;
    uint64_t last_activity;
} S;

/* ---- .vpk files that arrived in the last session (so the UI can offer to install them) ---- */
#define RX_VPK_MAX 8
static char s_vpk[RX_VPK_MAX][PATH_MAX_LEN];
static int  s_nvpk;
static bool s_vpk_fav;                    /* the sender of those files is one of our favorites */

/* ---- a received text message waiting for the UI ---- */
static bool s_msg_new;
static char s_msg_from[64], s_msg_text[HIST_TEXT_MAX];

/* ---- PIN brute-force protection ---- */
static int      s_pin_fails;
static uint64_t s_pin_locked_until;

void rx_init(void)
{
    if (!s_lock) s_lock = plat_mutex_create();
}

static void lock(void)   { plat_mutex_lock(s_lock); }
static void unlock(void) { plat_mutex_unlock(s_lock); }

/* ============================================================== file names */

/* Cuts a possibly half-finished UTF-8 sequence off the end. */
static size_t utf8_trim(const char *s, size_t n)
{
    if (n == 0) return 0;
    size_t i = n - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    unsigned char lead = (unsigned char)s[i];
    size_t need = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return (n - i < need) ? i : n;
}

/* Turns whatever the sender calls the file into a safe relative path:
 * no "..", no absolute paths, no characters the memory card's file system dislikes. */
void rx_sanitize_path(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    int depth = 0;
    const char *p = in;
    out[0] = 0;
    for (;;) {
        const char *q = p;
        while (*q && *q != '/' && *q != '\\') q++;
        char comp[128];
        size_t c = 0;
        for (const char *s = p; s < q && c < sizeof comp - 1; s++) {
            unsigned char ch = (unsigned char)*s;
            if (ch < 0x20 || ch == 0x7f || strchr("<>:\"|?*", ch)) ch = '_';
            comp[c++] = (char)ch;
        }
        c = utf8_trim(comp, c);
        while (c > 0 && (comp[c - 1] == '.' || comp[c - 1] == ' ')) c--;      /* FAT/exFAT do not like these */
        size_t lead = 0;
        while (lead < c && comp[lead] == ' ') lead++;
        if (lead) { memmove(comp, comp + lead, c - lead); c -= lead; }
        comp[c] = 0;
        if (c > 0 && depth < 8 && o + c + 2 < cap) {
            if (o) out[o++] = '/';
            memcpy(out + o, comp, c);
            o += c;
            out[o] = 0;
            depth++;
        }
        if (!*q) break;
        p = q + 1;
    }
    if (!o) ls_strlcpy(out, "file", cap);
}

/* Appends " (n)" before the extension until the name is free (on disk and inside this session). */
static void make_unique(char *path, size_t cap, const rx_file *files, int upto)
{
    char orig[PATH_MAX_LEN];
    ls_strlcpy(orig, path, sizeof orig);
    const char *slash = strrchr(orig, '/');
    const char *dot = strrchr(orig, '.');
    size_t stem = (dot && (!slash || dot > slash + 1)) ? (size_t)(dot - orig) : strlen(orig);
    for (int n = 1; n < 1000; n++) {
        bool clash = plat_path_info(path, NULL, NULL);
        for (int k = 0; k < upto && !clash; k++)
            if (!strcmp(files[k].path, path)) clash = true;
        if (!clash) return;
        snprintf(path, cap, "%.*s (%d)%s", (int)stem, orig, n, orig + stem);
    }
}

static bool is_hex64(const char *s)
{
    if (strlen(s) != 64) return false;
    for (; *s; s++)
        if (!isxdigit((unsigned char)*s)) return false;
    return true;
}

/* ============================================================== UI side */

bool rx_pending(rx_request *out)
{
    bool r = false;
    lock();
    if (s_q == Q_ASKING) {
        *out = s_qinfo;
        r = true;
    }
    unlock();
    return r;
}

void rx_decide(bool accept)
{
    lock();
    if (s_q == Q_ASKING) s_q = accept ? Q_ACCEPT : Q_REJECT;
    unlock();
}

bool rx_take_message(char *from, size_t from_cap, char *text, size_t text_cap)
{
    bool r = false;
    lock();
    if (s_msg_new) {
        ls_strlcpy(from, s_msg_from, from_cap);
        ls_strlcpy(text, s_msg_text, text_cap);
        s_msg_new = false;
        r = true;
    }
    unlock();
    return r;
}

bool rx_received_from_favorite(void)
{
    lock();
    bool r = s_vpk_fav;
    unlock();
    return r;
}

int rx_received_vpks(char out[][RX_PATH_CAP], int max)
{
    lock();
    int n = s_nvpk < max ? s_nvpk : max;
    for (int i = 0; i < n; i++) ls_strlcpy(out[i], s_vpk[i], RX_PATH_CAP);
    unlock();
    return n;
}

static void free_files_locked(void)
{
    free(S.files);
    S.files = NULL;
    S.nfiles = S.ndone = S.nfailed = S.nskipped = 0;
}

/* Ends the accepted session: history entry, UI state, memory. */
static void close_session_locked(xf_state st, const char *msg)
{
    if (!S.alive) return;
    S.alive = false;

    xfer_status xs;
    xfer_snapshot(&xs);
    hist_entry h;
    memset(&h, 0, sizeof h);
    h.time = (int64_t)time(NULL);
    h.outgoing = false;
    h.status = st == XF_DONE ? H_OK : st == XF_CANCELLED ? H_CANCELLED : H_FAILED;
    ls_strlcpy(h.peer, S.alias, sizeof h.peer);
    h.nfiles = S.ndone;
    h.bytes = xs.bytes_done;
    if (S.nfiles > 0) {
        if (S.nfiles > 1) snprintf(h.summary, sizeof h.summary, "%.60s (+%d more)", S.files[0].name, S.nfiles - 1);
        else ls_strlcpy(h.summary, S.files[0].name, sizeof h.summary);
        ls_strlcpy(h.path, S.files[0].path, sizeof h.path);
    }
    history_add(&h);

    xfer_end(st, "%s", msg);
    ls_log("receive session ended: %s", msg);
    if (S.refs == 0) free_files_locked();
}

void rx_tick(void)
{
    lock();
    if (S.alive) {
        uint64_t idle = plat_time_ms() - S.last_activity;
        if (*xfer_cancel_flag()) {
            close_session_locked(XF_CANCELLED, "Cancelled on this device");
        } else if (S.refs == 0 && idle > (S.nfailed ? IDLE_AFTER_FAIL_MS : IDLE_TIMEOUT_MS)) {
            close_session_locked(S.nfailed ? XF_FAILED : XF_CANCELLED, S.nfailed ? "Transfer failed" : "The sender stopped responding");
        }
    }
    unlock();
}

/* ============================================================== prepare-upload */

static void send_json_obj(http_res *res, int status, jw *w)
{
    if (w->oom) http_send_error(res, 500, "Out of memory");
    else http_send_json(res, status, w->buf);
}

void rx_handle_prepare(http_req *req, http_res *res)
{
    if (!req->body || !req->body_len) { http_send_error(res, 400, "Invalid body"); return; }
    char err[80];
    json_val *j = json_parse((const char *)req->body, req->body_len, err, sizeof err);
    const json_val *info = j ? json_get(j, "info") : NULL;
    const json_val *files = j ? json_get(j, "files") : NULL;
    const char *alias = json_str(json_get(info, "alias"), "");
    if (!j || j->type != J_OBJ || !*alias || !files || files->type != J_OBJ || files->n == 0 ||
        files->n > RX_MAX_FILES) {
        ls_log("prepare-upload from %s: invalid body", req->peer_ip);
        json_free(j);
        http_send_error(res, 400, "Invalid body");
        return;
    }

    ls_settings st;
    settings_snapshot(&st);

    /* ---- PIN ---- */
    if (st.pin[0]) {
        char pin[32] = "";
        bool have = http_query_get(req, "pin", pin, sizeof pin) && pin[0];
        lock();
        bool locked = plat_time_ms() < s_pin_locked_until;
        unlock();
        if (locked) { json_free(j); http_send_error(res, 429, "Too many attempts"); return; }
        if (!have) { json_free(j); http_send_error(res, 401, "PIN required"); return; }
        if (strcmp(pin, st.pin) != 0) {
            lock();
            if (++s_pin_fails >= 5) {
                s_pin_fails = 0;
                s_pin_locked_until = plat_time_ms() + 60000;
                ls_log("too many wrong PINs from %s - locked for a minute", req->peer_ip);
            }
            unlock();
            json_free(j);
            http_send_error(res, 401, "Invalid PIN");
            return;
        }
        lock();
        s_pin_fails = 0;
        unlock();
    }

    /* ---- is this a plain text message? ---- */
    int n = (int)files->n;
    const char *preview = NULL;
    bool is_text = false;
    if (n == 1) {
        const json_val *f0 = files->items[0];
        preview = json_str(json_get(f0, "preview"), NULL);
        is_text = preview && *preview && !strncmp(json_str(json_get(f0, "fileType"), ""), "text/", 5);
    }

    /* ---- parse the file list ---- */
    rx_file *fv = calloc((size_t)n, sizeof *fv);
    if (!fv) { json_free(j); http_send_error(res, 500, "Out of memory"); return; }
    uint64_t total = 0;
    bool bad = false;
    for (int i = 0; i < n && !bad; i++) {
        const json_val *f = files->items[i];
        const char *key = files->keys[i];
        const char *fname = json_str(json_get(f, "fileName"), "");
        double size = json_num(json_get(f, "size"), -1);
        const char *sha = json_str(json_get(f, "sha256"), "");
        if (!*fname || size < 0 || size > 1e13 || strlen(key) >= sizeof fv[i].id) { bad = true; break; }
        ls_strlcpy(fv[i].id, key, sizeof fv[i].id);
        rx_sanitize_path(fname, fv[i].name, sizeof fv[i].name);
        fv[i].size = (uint64_t)size;
        if (is_hex64(sha)) {
            for (int k = 0; k < 64; k++) fv[i].sha[k] = (char)tolower((unsigned char)sha[k]);
            fv[i].sha[64] = 0;
        }
        total += fv[i].size;
    }
    if (bad) {
        ls_log("prepare-upload from %s: invalid file list", req->peer_ip);
        free(fv);
        json_free(j);
        http_send_error(res, 400, "Invalid body");
        return;
    }

    /* ---- who is it? (the fingerprint in "info" is optional over HTTPS) ---- */
    char fp[65] = "";
    ls_strlcpy(fp, json_str(json_get(info, "fingerprint"), ""), sizeof fp);
    ls_peer known;
    if (!fp[0] && peers_find_by_ip(req->peer_ip, &known)) ls_strlcpy(fp, known.fingerprint, sizeof fp);

    /* ---- one transfer at a time ---- */
    lock();
    bool busy = S.files != NULL;               /* a cancelled session may still be draining */
    unlock();
    if (busy || vpk_busy() || !xfer_begin(false, alias)) {   /* (no uploads while a package is being installed) */
        ls_log("prepare-upload from %s (%s): busy", req->peer_ip, alias);    /* alias points into j: log first */
        free(fv);
        json_free(j);
        http_send_error(res, 409, "Blocked by another session");
        return;
    }

    /* ---- enough room on the card? (unknown free space = no check) ---- */
    if (!is_text && plat_mkdirs(st.receive_dir)) {              /* (a folder that cannot be created is reported later) */
        uint64_t freeb = plat_free_space(st.receive_dir);
        if (freeb != UINT64_MAX && total + (1u << 20) > freeb) {
            char need[24], have[24];
            fmt_size(total, need, sizeof need);
            fmt_size(freeb, have, sizeof have);
            ls_log("rejected %s: needs %s, only %s free", alias, need, have);
            hist_entry h;
            memset(&h, 0, sizeof h);
            h.time = (int64_t)time(NULL);
            h.status = H_FAILED;
            h.nfiles = n;
            ls_strlcpy(h.peer, alias, sizeof h.peer);
            snprintf(h.summary, sizeof h.summary, "not enough space (%s needed, %s free)", need, have);
            history_add(&h);
            xfer_end(XF_FAILED, "Not enough space for the files from %.40s (%s needed, %s free)", alias, need, have);
            free(fv);
            json_free(j);
            http_send_error(res, 507, "Not enough free space on the Vita");
            return;
        }
    }

    bool auto_ok = st.auto_accept == AUTO_EVERYONE || (st.auto_accept == AUTO_FAVORITES && fp[0] && settings_is_fav(fp));

    /* ---- ask the user ---- */
    bool accepted = auto_ok;
    if (!auto_ok) {
        rx_request q;
        memset(&q, 0, sizeof q);
        ls_strlcpy(q.alias, alias, sizeof q.alias);
        ls_strlcpy(q.model, json_str(json_get(info, "deviceModel"), ""), sizeof q.model);
        ls_strlcpy(q.ip, req->peer_ip, sizeof q.ip);
        ls_strlcpy(q.fp, fp, sizeof q.fp);
        q.nfiles = n;
        q.total = total;
        q.is_text = is_text;
        if (is_text) ls_strlcpy(q.text, preview, sizeof q.text);
        for (int i = 0; i < n && i < 4; i++) ls_strlcpy(q.names[i], fv[i].name, sizeof q.names[i]);
        q.on_exists = st.on_exists;
        if (!is_text) {
            for (int i = 0; i < n; i++) {
                char p[PATH_MAX_LEN];
                snprintf(p, sizeof p, "%s/%.200s", st.receive_dir, fv[i].name);
                if (plat_path_info(p, NULL, NULL)) q.nexist++;
            }
        }

        lock();
        s_qinfo = q;
        s_q = Q_ASKING;
        unlock();
        ls_log("%s wants to send %d file(s), %llu bytes - waiting for you", alias, n, (unsigned long long)total);

        uint64_t deadline = plat_time_ms() + ASK_TIMEOUT_MS;
        for (;;) {
            lock();
            int state = s_q;
            unlock();
            if (state != Q_ASKING) break;
            if (g_quit || plat_time_ms() > deadline) break;
            plat_sleep_ms(50);
        }
        lock();
        accepted = s_q == Q_ACCEPT;
        s_q = Q_NONE;
        unlock();
    }

    if (!accepted) {
        hist_entry h;
        memset(&h, 0, sizeof h);
        h.time = (int64_t)time(NULL);
        h.status = H_REJECTED;
        h.is_text = is_text;
        h.nfiles = n;
        ls_strlcpy(h.peer, alias, sizeof h.peer);
        ls_strlcpy(h.summary, fv[0].name, sizeof h.summary);
        history_add(&h);
        xfer_end_silent();
        ls_log("rejected the request from %s", alias);
        free(fv);
        json_free(j);
        http_send_error(res, 403, "Rejected");
        return;
    }

    /* ---- text message: nothing to download ---- */
    if (is_text) {
        hist_entry h;
        memset(&h, 0, sizeof h);
        h.time = (int64_t)time(NULL);
        h.status = H_OK;
        h.is_text = true;
        h.nfiles = 1;
        ls_strlcpy(h.peer, alias, sizeof h.peer);
        ls_strlcpy(h.text, preview, sizeof h.text);
        h.text[utf8_trim(h.text, strlen(h.text))] = 0;
        ls_strlcpy(h.summary, h.text, sizeof h.summary);
        h.summary[utf8_trim(h.summary, strlen(h.summary))] = 0;
        history_add(&h);
        lock();
        ls_strlcpy(s_msg_from, alias, sizeof s_msg_from);
        ls_strlcpy(s_msg_text, h.text, sizeof s_msg_text);
        s_msg_new = true;
        unlock();
        xfer_end_silent();
        ls_log("text message from %s", alias);
        free(fv);
        json_free(j);
        http_send(res, 204, NULL, NULL, 0);
        return;
    }

    /* ---- accepted: work out where each file goes and hand out tokens ---- */
    if (!plat_mkdirs(st.receive_dir)) {
        ls_log("cannot create the receive folder %s", st.receive_dir);
        xfer_end(XF_FAILED, "Cannot create folder %s", st.receive_dir);
        free(fv);
        json_free(j);
        http_send_error(res, 500, "Cannot create the receive folder");
        return;
    }
    /* What to do when the name is taken: keep both (the new file gets " (1)"), skip it, or overwrite the old one.
     * Inside one request two files with the same name never replace each other. */
    int nkept = 0, nskipped = 0;
    uint64_t kept_bytes = 0;
    for (int i = 0; i < n; i++) {
        rx_file f = fv[i];
        snprintf(f.path, sizeof f.path, "%s/%.200s", st.receive_dir, f.name);
        bool is_dir = false;
        bool exists = plat_path_info(f.path, &is_dir, NULL);
        if (exists && st.on_exists == EXISTS_SKIP) { nskipped++; continue; }
        if (!exists || st.on_exists != EXISTS_OVERWRITE || is_dir) make_unique(f.path, sizeof f.path, fv, nkept);
        else for (int k = 0; k < nkept; k++)
            if (!strcmp(fv[k].path, f.path)) { make_unique(f.path, sizeof f.path, fv, nkept); break; }
        uint8_t r[16];
        tls_random(NULL, r, sizeof r);
        ls_hex(r, sizeof r, f.token);
        f.st = RF_PENDING;
        kept_bytes += f.size;
        fv[nkept++] = f;                                    /* skipped files simply drop out of the list */
    }
    if (nkept == 0) {                                       /* everything is there already: nothing to receive */
        hist_entry h;
        memset(&h, 0, sizeof h);
        h.time = (int64_t)time(NULL);
        h.status = H_OK;
        h.nfiles = 0;
        ls_strlcpy(h.peer, alias, sizeof h.peer);
        snprintf(h.summary, sizeof h.summary, "%d file%s skipped (already there)", nskipped, nskipped == 1 ? "" : "s");
        history_add(&h);
        xfer_end(XF_DONE, "Nothing new from %.40s: %d file%s already there", alias, nskipped, nskipped == 1 ? " is" : "s are");
        ls_log("all %d file(s) from %s exist already - skipped", nskipped, alias);
        free(fv);
        json_free(j);
        http_send(res, 204, NULL, NULL, 0);
        return;
    }
    total = kept_bytes;
    n = nkept;
    uint8_t sr[16];
    tls_random(NULL, sr, sizeof sr);
    bool from_fav = fp[0] && settings_is_fav(fp);

    lock();
    ls_hex(sr, sizeof sr, S.sid);
    ls_strlcpy(S.ip, req->peer_ip, sizeof S.ip);
    ls_strlcpy(S.alias, alias, sizeof S.alias);
    S.files = fv;
    S.nfiles = n;
    S.ndone = S.nfailed = 0;
    S.nskipped = nskipped;
    S.refs = 0;
    s_nvpk = 0;
    s_vpk_fav = from_fav;
    S.alive = true;
    S.last_activity = plat_time_ms();
    unlock();

    xfer_set_totals(n, total);
    xfer_set_state(XF_ACTIVE);
    xfer_set_message("Receiving from %s", alias);
    ls_log("accepted %d file(s) from %s -> %s%s", n, alias, st.receive_dir, nskipped ? " (some skipped: already there)" : "");
    if (nskipped) xfer_set_message("Receiving from %s (%d skipped, already there)", alias, nskipped);

    jw w;
    jw_init(&w);
    jw_obj(&w);
    jw_kstr(&w, "sessionId", S.sid);
    jw_key(&w, "files");
    jw_obj(&w);
    for (int i = 0; i < n; i++) jw_kstr(&w, fv[i].id, fv[i].token);
    jw_end_obj(&w);
    jw_end_obj(&w);
    send_json_obj(res, 200, &w);
    jw_free(&w);
    json_free(j);
}

/* ============================================================== upload */

static void upload_done_locked(void)
{
    S.refs--;
    S.last_activity = plat_time_ms();
    if (S.alive && S.ndone + S.nfailed >= S.nfiles) {
        if (S.nfailed == 0) {
            char msg[80];
            if (S.nskipped) snprintf(msg, sizeof msg, "%d file%s received, %d skipped (already there)", S.ndone, S.ndone == 1 ? "" : "s", S.nskipped);
            else snprintf(msg, sizeof msg, "%d file%s received", S.ndone, S.ndone == 1 ? "" : "s");
            close_session_locked(XF_DONE, msg);
        } else {
            close_session_locked(XF_FAILED, "Some files could not be received");
        }
    }
    if (!S.alive && S.refs == 0 && S.files) free_files_locked();
}

void rx_handle_upload(http_req *req, http_res *res)
{
    char sid[80] = "", fid[100] = "", token[80] = "";
    if (!http_query_get(req, "sessionId", sid, sizeof sid) || !http_query_get(req, "fileId", fid, sizeof fid) ||
        !http_query_get(req, "token", token, sizeof token)) {
        http_drain(req, 4096);
        http_send_error(res, 400, "Missing parameters");
        return;
    }

    /* ---- find and claim the file ---- */
    int idx = -1;
    int code = 0;
    const char *why = "";
    char path[PATH_MAX_LEN] = "", name[200] = "", want_sha[65] = "";
    uint64_t size = 0;
    lock();
    if (!S.alive || strcmp(sid, S.sid) != 0 || strcmp(req->peer_ip, S.ip) != 0) {
        code = 403; why = "Invalid session or IP address";
    } else {
        for (int i = 0; i < S.nfiles; i++)
            if (!strcmp(S.files[i].id, fid)) { idx = i; break; }
        if (idx < 0 || strcmp(S.files[idx].token, token) != 0) {
            code = 403; why = "Invalid token";
        } else if (S.files[idx].st != RF_PENDING && S.files[idx].st != RF_FAILED) {
            code = 409; why = "File already transferred";
        } else {
            if (S.files[idx].st == RF_FAILED) S.nfailed--;
            S.files[idx].st = RF_ACTIVE;
            S.refs++;
            S.last_activity = plat_time_ms();
            ls_strlcpy(path, S.files[idx].path, sizeof path);
            ls_strlcpy(name, S.files[idx].name, sizeof name);
            ls_strlcpy(want_sha, S.files[idx].sha, sizeof want_sha);
            size = S.files[idx].size;
        }
    }
    unlock();
    if (code) {
        http_drain(req, 65536);
        http_send_error(res, code, why);
        return;
    }

    /* ---- stream it to disk ---- */
    req->cancel = xfer_cancel_flag();
    xfer_set_current(name);
    int         final_code = 200;
    const char *final_why = "";
    FILE       *f = NULL;
    char        part[PATH_MAX_LEN + 8];
    snprintf(part, sizeof part, "%s.part", path);
    uint8_t    *rb = malloc(READ_CHUNK);
    uint8_t    *wbuf = malloc(WRITE_BUF);
    size_t      wlen = 0;
    uint64_t    got = 0;
    ls_sha256  *hash = want_sha[0] ? sha256_new() : NULL;

    char dir[PATH_MAX_LEN];
    ls_strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; plat_mkdirs(dir); }

    if (!rb || !wbuf) {
        final_code = 500; final_why = "Out of memory";
    } else if (!req->chunked && req->content_length != (int64_t)size) {
        final_code = 400; final_why = "Size does not match the announced size";
    } else if (!(f = fopen(part, "wb"))) {
        final_code = 500; final_why = "Cannot create the file";
        ls_log("cannot create %s", part);
    } else {
        while (final_code == 200) {
            int r = http_stream_read(req, rb, READ_CHUNK);
            if (r == 0) break;
            if (r < 0) { final_code = 500; final_why = *xfer_cancel_flag() ? "Cancelled" : "Connection lost"; break; }
            uint64_t room = size > got ? size - got : 0;
            if ((uint64_t)r > room) { final_code = 400; final_why = "More data than announced"; break; }
            if (hash) sha256_update(hash, rb, (size_t)r);
            if (wlen + (size_t)r > WRITE_BUF) {
                if (fwrite(wbuf, 1, wlen, f) != wlen) { final_code = 500; final_why = "Write error (disk full?)"; break; }
                wlen = 0;
            }
            memcpy(wbuf + wlen, rb, (size_t)r);
            wlen += (size_t)r;
            got += (uint64_t)r;
            xfer_add_bytes((uint64_t)r);
            if (*xfer_cancel_flag()) { final_code = 500; final_why = "Cancelled"; break; }
        }
        if (final_code == 200 && wlen && fwrite(wbuf, 1, wlen, f) != wlen) {
            final_code = 500; final_why = "Write error (disk full?)";
        }
        if (fclose(f) != 0 && final_code == 200) { final_code = 500; final_why = "Write error (disk full?)"; }
        f = NULL;
        if (final_code == 200 && got != size) { final_code = 400; final_why = "Fewer bytes than announced"; }
    }
    free(rb);
    free(wbuf);

    if (final_code == 200 && hash) {
        char have[65];
        sha256_finish_hex(hash, have);
        hash = NULL;
        if (strcmp(have, want_sha) != 0) { final_code = 422; final_why = "Checksum mismatch"; }
    }
    if (hash) sha256_discard(hash);

    if (final_code == 200) {
        if (!plat_rename(part, path)) {
            plat_remove(path);
            if (!plat_rename(part, path)) { final_code = 500; final_why = "Cannot move the file into place"; }
        }
    }
    if (final_code != 200) plat_remove(part);

    /* ---- book-keeping ---- */
    lock();
    if (S.files && idx < S.nfiles) {
        S.files[idx].st = final_code == 200 ? RF_DONE : RF_FAILED;
        if (final_code == 200) {
            S.ndone++;
            if (vpk_has_extension(path) && s_nvpk < RX_VPK_MAX) ls_strlcpy(s_vpk[s_nvpk++], path, sizeof s_vpk[0]);
        } else {
            S.nfailed++;
        }
    }
    unlock();
    if (final_code == 200) {
        xfer_file_done();
        ls_log("received %s (%llu bytes)", name, (unsigned long long)size);
    } else {
        ls_log("receiving %s failed: %s", name, final_why);
        http_drain(req, 65536);
    }
    if (final_code == 200) http_send(res, 200, NULL, NULL, 0);
    else http_send_error(res, final_code, final_why);

    lock();
    upload_done_locked();
    unlock();
}

/* ============================================================== cancel */

void rx_handle_cancel(http_req *req, http_res *res)
{
    char sid[80] = "";
    http_query_get(req, "sessionId", sid, sizeof sid);
    lock();
    bool mine = S.alive && !strcmp(sid, S.sid) && !strcmp(req->peer_ip, S.ip);
    if (mine) {
        char msg[120];
        snprintf(msg, sizeof msg, "Cancelled by %s", S.alias);
        *xfer_cancel_flag() = true;                       /* running uploads stop at their next read */
        close_session_locked(XF_CANCELLED, msg);
    }
    unlock();
    if (mine) http_send(res, 200, NULL, NULL, 0);
    else http_send_error(res, 403, "Unknown session");
}
