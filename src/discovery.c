/*
 * discovery.c - Finding other devices: the multicast announcement (224.0.0.167:53317) - a burst at start, then every 30 s - and its listener.
 * A device that announces itself is answered with POST /register (or, if that fails, a UDP reply with announce:false).
 * Registrations run on short-lived worker threads; found devices go into the list in peers.c.
 *
 * Author: trashman (VitaSend)
 */
#include "discovery.h"
#include "http.h"
#include "identity.h"
#include "json.h"
#include "peers.h"
#include "plat.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PERIODIC_ANNOUNCE_MS 30000
#define MAX_REGISTER_JOBS    3
#define REGISTER_GAP_MS      2500       /* don't re-register with the same peer more often than this */
#define REGISTER_TIMEOUT_MS  6000
#define WORKER_STACK_BYTES   (256 * 1024)

static ls_sock          s_sock = LS_BAD_SOCK;
static char             s_announce_ip[16] = LS_MULTICAST_IP;
static uint16_t         s_announce_port = LS_DEFAULT_PORT;
static uint16_t         s_listen_port = LS_DEFAULT_PORT;
static volatile bool    s_want_announce;
static volatile bool    s_net_changed;
static volatile int     s_udp_replies;       /* "announce:false" replies queued by register workers */
static volatile bool    s_listening;
static plat_mutex      *s_lock;
static int              s_jobs;

void discovery_configure(const char *announce_ip, uint16_t announce_port, uint16_t listen_port)
{
    if (announce_ip && *announce_ip) ls_strlcpy(s_announce_ip, announce_ip, sizeof s_announce_ip);
    if (announce_port) s_announce_port = announce_port;
    if (listen_port) s_listen_port = listen_port;
}

void discovery_request_announce(void)  { s_want_announce = true; }
void discovery_network_changed(void)   { s_net_changed = true; }
bool discovery_listening(void)         { return s_listening; }

/* ------------------------------------------------------------------ sending */

static void send_udp_message(int announce_mode)
{
    if (s_sock < 0) return;
    jw w;
    jw_init(&w);
    identity_json(&w, true, announce_mode);
    if (!w.oom) {
        int r = plat_udp_sendto(s_sock, w.buf, w.len, s_announce_ip, s_announce_port);
        if (r < 0) ls_log("UDP send to %s:%u failed", s_announce_ip, s_announce_port);
    }
    jw_free(&w);
}

/* ------------------------------------------------------------------ registering with a peer */

typedef struct {
    ls_peer peer;                            /* what the announcement told us (ip/port/protocol/...) */
} reg_job;

static void register_thread(void *arg)
{
    reg_job *j = arg;
    ls_peer *p = &j->peer;

    jw w;
    jw_init(&w);
    identity_json(&w, true, 0);

    http_resp resp;
    bool ok = !w.oom && http_request(p->ip, p->port, p->https, "POST", "/api/localsend/v2/register",
                                     "application/json", w.buf, w.len, REGISTER_TIMEOUT_MS, &resp);
    jw_free(&w);

    if (ok && resp.status == 200) {
        /* The reply tells us who answered - refresh our entry (it may carry a nicer alias/model). */
        char err[80];
        json_val *r = json_parse(resp.body, resp.body_len, err, sizeof err);
        /* Only trust the reply if it comes from the device that announced itself. */
        if (r && r->type == J_OBJ && !strcmp(json_str(json_get(r, "fingerprint"), ""), p->fingerprint)) {
            const char *a = json_str(json_get(r, "alias"), NULL);
            if (a && *a) ls_strlcpy(p->alias, a, sizeof p->alias);
            ls_strlcpy(p->model, json_str(json_get(r, "deviceModel"), p->model), sizeof p->model);
            ls_strlcpy(p->type, json_str(json_get(r, "deviceType"), p->type), sizeof p->type);
        }
        json_free(r);
        peers_upsert(p);
        ls_log("registered with %s (%s:%u, %s)", p->alias, p->ip, p->port, p->https ? "https" : "http");
    } else {
        if (ok)
            ls_log("register with %s:%u -> HTTP %d, replying via multicast instead", p->ip, p->port, resp.status);
        else
            ls_log("register with %s:%u failed, replying via multicast instead", p->ip, p->port);
        plat_mutex_lock(s_lock);
        s_udp_replies++;                     /* the discovery thread sends the UDP answer */
        plat_mutex_unlock(s_lock);
    }
    if (ok) http_resp_free(&resp);

    free(j);
    plat_mutex_lock(s_lock);
    s_jobs--;
    plat_mutex_unlock(s_lock);
}

static void start_register(const ls_peer *p)
{
    plat_mutex_lock(s_lock);
    bool busy = s_jobs >= MAX_REGISTER_JOBS;
    if (!busy) s_jobs++;
    plat_mutex_unlock(s_lock);
    if (busy) return;

    reg_job *j = malloc(sizeof *j);
    if (j) j->peer = *p;
    if (!j || !ls_spawn("ls_register", register_thread, j, WORKER_STACK_BYTES)) {
        free(j);
        plat_mutex_lock(s_lock);
        s_jobs--;
        plat_mutex_unlock(s_lock);
    }
}

/* ------------------------------------------------------------------ manual "add by address" */

typedef struct {
    char     ip[16];
    uint16_t port;
} manual_job;

static char s_manual_msg[120];
static bool s_manual_new;
static bool s_manual_busy;

static void manual_say(const char *fmt, ...) LS_PRINTF(1, 2);
static void manual_say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    plat_mutex_lock(s_lock);
    vsnprintf(s_manual_msg, sizeof s_manual_msg, fmt, ap);
    s_manual_new = true;
    s_manual_busy = false;
    plat_mutex_unlock(s_lock);
    va_end(ap);
}

static void manual_thread(void *arg)
{
    manual_job *m = arg;
    jw w;
    jw_init(&w);
    identity_json(&w, true, 0);

    http_resp resp;
    memset(&resp, 0, sizeof resp);
    bool https = true, ok = false;
    for (int attempt = 0; attempt < 2 && !w.oom; attempt++) {
        https = attempt == 0;
        ok = http_request(m->ip, m->port, https, "POST", "/api/localsend/v2/register", "application/json",
                          w.buf, w.len, REGISTER_TIMEOUT_MS, &resp);
        if (ok && resp.status == 200) break;
        if (ok) http_resp_free(&resp);
        ok = false;
    }
    jw_free(&w);

    if (!ok) {
        manual_say("Nothing answered at %s:%u (is LocalSend open there?)", m->ip, m->port);
    } else {
        char err[80];
        json_val *r = json_parse(resp.body, resp.body_len, err, sizeof err);
        const char *alias = r ? json_str(json_get(r, "alias"), "") : "";
        const char *fp = r ? json_str(json_get(r, "fingerprint"), "") : "";
        if (!r || r->type != J_OBJ || !*alias || !*fp || strlen(fp) >= sizeof(((ls_peer *)0)->fingerprint)) {
            manual_say("%s:%u answered, but it does not look like LocalSend", m->ip, m->port);
        } else if (!strcmp(fp, g_id.fingerprint)) {
            manual_say("That is this Vita itself");
        } else {
            ls_peer p;
            memset(&p, 0, sizeof p);
            ls_strlcpy(p.alias, alias, sizeof p.alias);
            ls_strlcpy(p.fingerprint, fp, sizeof p.fingerprint);
            ls_strlcpy(p.model, json_str(json_get(r, "deviceModel"), ""), sizeof p.model);
            ls_strlcpy(p.type, json_str(json_get(r, "deviceType"), "desktop"), sizeof p.type);
            ls_strlcpy(p.ip, m->ip, sizeof p.ip);
            p.port = m->port;
            p.https = https;
            p.download = json_bool(json_get(r, "download"), false);
            peers_upsert(&p);
            ls_log("added %s (%s:%u, %s) by address", p.alias, p.ip, p.port, https ? "https" : "http");
            manual_say("Added %s", p.alias);
        }
        json_free(r);
        http_resp_free(&resp);
    }
    free(m);
}

bool discovery_add_manual(const char *ip, uint16_t port)
{
    if (!s_lock) return false;
    plat_mutex_lock(s_lock);
    bool busy = s_manual_busy;
    if (!busy) s_manual_busy = true;
    plat_mutex_unlock(s_lock);
    if (busy) return false;
    manual_job *m = malloc(sizeof *m);
    if (m) {
        ls_strlcpy(m->ip, ip, sizeof m->ip);
        m->port = port;
    }
    if (!m || !ls_spawn("ls_manual", manual_thread, m, WORKER_STACK_BYTES)) {
        free(m);
        plat_mutex_lock(s_lock);
        s_manual_busy = false;
        plat_mutex_unlock(s_lock);
        return false;
    }
    return true;
}

bool discovery_take_manual_result(char *out, size_t cap)
{
    if (!s_lock) return false;
    plat_mutex_lock(s_lock);
    bool have = s_manual_new;
    if (have) {
        ls_strlcpy(out, s_manual_msg, cap);
        s_manual_new = false;
    }
    plat_mutex_unlock(s_lock);
    return have;
}

/* ------------------------------------------------------------------ receiving */

static void handle_datagram(const char *data, size_t len, const ls_endpoint *from)
{
    char err[80];
    json_val *j = json_parse(data, len, err, sizeof err);
    if (!j || j->type != J_OBJ) {
        ls_log("ignored UDP packet from %s (not JSON)", from->ip);
        json_free(j);
        return;
    }

    const char *alias = json_str(json_get(j, "alias"), NULL);
    const char *fp    = json_str(json_get(j, "fingerprint"), NULL);
    if (!alias || !*alias || !fp || !*fp || strlen(fp) >= sizeof(((ls_peer *)0)->fingerprint)) {
        json_free(j);
        return;
    }
    if (!strcmp(fp, g_id.fingerprint)) {                 /* our own announcement coming back */
        json_free(j);
        return;
    }

    ls_peer p;
    memset(&p, 0, sizeof p);
    ls_strlcpy(p.alias, alias, sizeof p.alias);
    ls_strlcpy(p.fingerprint, fp, sizeof p.fingerprint);
    ls_strlcpy(p.model, json_str(json_get(j, "deviceModel"), ""), sizeof p.model);
    ls_strlcpy(p.type, json_str(json_get(j, "deviceType"), "desktop"), sizeof p.type);
    ls_strlcpy(p.ip, from->ip, sizeof p.ip);
    double port = json_num(json_get(j, "port"), LS_DEFAULT_PORT);
    p.port = (port >= 1 && port <= 65535) ? (uint16_t)port : LS_DEFAULT_PORT;
    p.https = strcmp(json_str(json_get(j, "protocol"), "https"), "http") != 0;
    p.download = json_bool(json_get(j, "download"), false);
    /* v2 says "announce", v1 said "announcement" */
    const json_val *a = json_get(j, "announce");
    if (!a) a = json_get(j, "announcement");
    bool announce = json_bool(a, false);
    json_free(j);

    bool is_new = peers_upsert(&p);
    if (is_new) ls_log("found %s (%s) %s:%u via multicast", p.alias, p.type, p.ip, p.port);

    if (announce && peers_claim_register(p.fingerprint, REGISTER_GAP_MS))
        start_register(&p);
}

/* ------------------------------------------------------------------ main loop */

static void open_socket(void)
{
    if (s_sock >= 0) {
        plat_sock_close(s_sock);
        s_sock = LS_BAD_SOCK;
        s_listening = false;
    }
    char ip[16] = "";
    plat_net_local_ip(ip);

    s_sock = plat_udp_open(s_listen_port);
    if (s_sock >= 0) {
        s_listening = true;
        if (plat_udp_join_group(s_sock, LS_MULTICAST_IP, ip[0] ? ip : NULL))
            ls_log("listening for announcements on %s:%u", LS_MULTICAST_IP, s_listen_port);
        else
            ls_log("could not join multicast group (we can still announce; peers will find us)");
    } else {
        ls_log("UDP port %u unavailable (%s) - announce-only mode", s_listen_port, plat_net_error_text());
        s_sock = plat_udp_open(0);
    }
}

static void discovery_thread(void *arg)
{
    (void)arg;
    static const unsigned burst_ms[4] = { 0, 150, 700, 2500 };   /* the official app does this too */
    uint8_t buf[2048];

    open_socket();
    uint64_t burst_start = plat_time_ms();
    int burst_i = 0;
    uint64_t next_periodic = plat_time_ms() + PERIODIC_ANNOUNCE_MS;

    while (!g_quit) {
        uint64_t now = plat_time_ms();

        if (s_net_changed) {
            s_net_changed = false;
            open_socket();
            burst_start = now;
            burst_i = 0;
        }
        if (s_want_announce) {
            s_want_announce = false;
            burst_start = now;
            burst_i = 0;
        }

        bool busy = false;
        for (int k = 0; k < 16 && s_sock >= 0; k++) {          /* drain what has arrived */
            ls_endpoint from;
            int n = plat_udp_recvfrom(s_sock, buf, sizeof buf - 1, &from);
            if (n <= 0) break;
            buf[n] = 0;
            handle_datagram((const char *)buf, (size_t)n, &from);
            busy = true;
        }

        if (burst_i < 4 && now - burst_start >= burst_ms[burst_i]) {
            send_udp_message(1);
            burst_i++;
            busy = true;
        }
        if (now >= next_periodic) {
            next_periodic = now + PERIODIC_ANNOUNCE_MS;
            send_udp_message(1);                               /* a single re-announce */
            busy = true;
        }
        for (;;) {                                             /* UDP answers queued by register workers */
            plat_mutex_lock(s_lock);
            bool more = s_udp_replies > 0;
            if (more) s_udp_replies--;
            plat_mutex_unlock(s_lock);
            if (!more) break;
            send_udp_message(2);
            busy = true;
        }

        if (!busy) plat_sleep_ms(20);
    }
    if (s_sock >= 0) plat_sock_close(s_sock);
    s_sock = LS_BAD_SOCK;
}

bool discovery_start(void)
{
    s_lock = plat_mutex_create();
    return ls_spawn("ls_discovery", discovery_thread, NULL, WORKER_STACK_BYTES);
}
