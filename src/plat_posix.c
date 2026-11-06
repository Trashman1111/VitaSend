/*
 * plat_posix.c - host (Linux/macOS) implementation of plat.h.
 * Only used for the optional PC test build in host/. The Vita build uses plat_vita.c.
 *
 * Author: trashman (VitaSend)
 */
#define _GNU_SOURCE
#include "plat.h"
#include "util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static char s_data_dir[256] = "./ls-data";

void plat_posix_set_data_dir(const char *dir)
{
    snprintf(s_data_dir, sizeof s_data_dir, "%s", dir);
}

const char *plat_data_dir(void)
{
    return s_data_dir;
}

bool plat_mkdirs(const char *dir)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    struct stat st;
    return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode);
}

static uint64_t s_fake_free = UINT64_MAX;

void plat_posix_set_free_space(uint64_t bytes) { s_fake_free = bytes; }

uint64_t plat_free_space(const char *path)
{
    if (s_fake_free != UINT64_MAX) return s_fake_free;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (;;) {                                       /* walk up to a folder that exists */
        struct statvfs v;
        if (statvfs(tmp, &v) == 0) return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
        char *slash = strrchr(tmp, '/');
        if (!slash || slash == tmp) return UINT64_MAX;
        *slash = 0;
    }
}

static char s_browse_start[256];

void plat_posix_set_browse_start(const char *dir)
{
    snprintf(s_browse_start, sizeof s_browse_start, "%s", dir);
}

const char *plat_browse_start(void)
{
    return s_browse_start[0] ? s_browse_start : NULL;
}

int plat_list_roots(char out[][16], int max)
{
    if (max < 1) return 0;
    snprintf(out[0], 16, "/");
    return 1;
}

int plat_list_dir(const char *path, plat_dirent **out)
{
    DIR *d = opendir(path);
    if (!d) return -1;
    size_t cap = 32, n = 0;
    plat_dirent *v = malloc(cap * sizeof *v);
    if (!v) { closedir(d); return -1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            cap *= 2;
            plat_dirent *nv = realloc(v, cap * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        plat_dirent *o = &v[n];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", e->d_name);
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0) {
            o->is_dir = S_ISDIR(st.st_mode);
            o->size = o->is_dir ? 0 : (uint64_t)st.st_size;
        }
        n++;
    }
    closedir(d);
    *out = v;
    return (int)n;
}

bool plat_path_info(const char *path, bool *is_dir, uint64_t *size)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    if (size) *size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    return true;
}

bool plat_rename(const char *from, const char *to)
{
    if (getenv("LS_FAIL_RENAME")) return false;          /* tests: behave like two memory cards (a rename cannot cross them) */
    return rename(from, to) == 0;
}
bool plat_remove(const char *path)                 { return unlink(path) == 0; }
bool plat_rmdir(const char *path)                  { return rmdir(path) == 0; }

/* ---- installer (PC test build: "installs" by moving the folder to <data>/installed/<TITLEID>) ---- */
const char *plat_pkg_dir(void)
{
    static char dir[300];
    snprintf(dir, sizeof dir, "%s/pkg", s_data_dir);
    return dir;
}

const char *plat_own_title_id(void) { return "LSND00001"; }

int plat_promote(const char *dir, char *why, size_t cap)
{
    { const char *sl = getenv("LS_SLOW_PROMOTE_MS"); if (sl && atoi(sl) > 0) usleep((useconds_t)atoi(sl) * 1000u); }   /* the real installer takes a while */
    if (getenv("LS_FAKE_PROMOTE_FAIL")) { snprintf(why, cap, "promoter failed (simulated) 0x80870001"); return -1; }
    char p[400];
    struct stat st;
    snprintf(p, sizeof p, "%s/sce_sys/package/head.bin", dir);
    if (stat(p, &st) != 0 || st.st_size != 1072) { snprintf(why, cap, "head.bin missing"); return -1; }
    snprintf(p, sizeof p, "%s/sce_sys/param.sfo", dir);
    uint8_t *sfo = NULL; size_t n = 0;
    FILE *f = fopen(p, "rb");
    if (!f) { snprintf(why, cap, "param.sfo missing"); return -1; }
    sfo = malloc(4096);
    n = sfo ? fread(sfo, 1, 4096, f) : 0;
    fclose(f);
    char tid[16] = "UNKNOWN";
    if (n > 32) {
        uint32_t ko = sfo[8] | (sfo[9] << 8), dof = sfo[12] | (sfo[13] << 8), cnt = sfo[16];
        for (uint32_t i = 0; i < cnt && 20 + i * 16 + 16 <= n; i++) {
            const uint8_t *e = sfo + 20 + i * 16;
            uint32_t k = e[0] | (e[1] << 8), d = e[12] | (e[13] << 8);
            if (ko + k < n && strcmp((const char *)sfo + ko + k, "TITLE_ID") == 0 && dof + d + 9 <= n) {
                memcpy(tid, sfo + dof + d, 9);
                tid[9] = 0;
            }
        }
    }
    free(sfo);
    char dst[512], parent[400];
    snprintf(parent, sizeof parent, "%s/installed", s_data_dir);
    plat_mkdirs(parent);
    snprintf(dst, sizeof dst, "%s/%s", parent, tid);
    char cmd[900];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dst);
    if (system(cmd) != 0) { /* ignore */ }
    if (rename(dir, dst) != 0) { snprintf(why, cap, "cannot move into place"); return -1; }
    return 0;
}

void plat_init(void) {}

void plat_battery(int *percent, bool *charging)
{
    const char *e = getenv("LS_FAKE_BATTERY");                 /* test knob; a real PC has no Vita battery */
    *percent = e ? atoi(e) : 100;
    *charging = getenv("LS_FAKE_CHARGING") != NULL;
}

bool plat_local_date(int *ymd)
{
    const char *fake = getenv("LS_FAKE_DATE");                  /* test knob: "YYYY-MM-DD" */
    int y, m, d;
    if (fake && sscanf(fake, "%d-%d-%d", &y, &m, &d) == 3) { *ymd = y * 10000 + m * 100 + d; return true; }
    time_t t = time(NULL);
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return false;
    *ymd = (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
    return true;
}

bool plat_local_clock(int *hour, int *minute)
{
    const char *fake = getenv("LS_FAKE_CLOCK");                 /* test knob: "HH:MM" */
    int h, m;
    if (fake && sscanf(fake, "%d:%d", &h, &m) == 2) { *hour = h; *minute = m; return true; }
    time_t t = time(NULL);
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return false;
    *hour = tmv.tm_hour;
    *minute = tmv.tm_min;
    return true;
}

void plat_chime(void) { ls_log("sound: finished chime played"); }

bool plat_can_launch(void) { return true; }

bool plat_launch_app(const char *titleid)
{
    ls_log("launch: %s", titleid);
    return true;
}

void plat_keep_awake(void)
{
    /* test knob: make long jobs (the package installer calls this once per unpacked file) slow enough to cancel */
    const char *slow = getenv("LS_SLOW_KEEPAWAKE_MS");
    if (slow) usleep((useconds_t)atoi(slow) * 1000u);
}

uint64_t plat_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void plat_sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

bool plat_random(void *buf, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    return got == len;
}

/* ------------------------------------------------------------------ threads */

struct plat_mutex { pthread_mutex_t m; };

plat_mutex *plat_mutex_create(void)
{
    plat_mutex *m = malloc(sizeof *m);
    if (m) pthread_mutex_init(&m->m, NULL);
    return m;
}
void plat_mutex_lock(plat_mutex *m)   { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(&m->m); }

typedef struct { void (*fn)(void *); void *arg; } tramp;

static void *tramp_main(void *p)
{
    tramp t = *(tramp *)p;
    free(p);
    t.fn(t.arg);
    return NULL;
}

bool plat_thread_spawn(const char *name, void (*fn)(void *), void *arg, size_t stack_bytes)
{
    (void)name;
    tramp *t = malloc(sizeof *t);
    if (!t) return false;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (stack_bytes >= 64 * 1024) pthread_attr_setstacksize(&a, stack_bytes);
    pthread_t th;
    int r = pthread_create(&th, &a, tramp_main, t);
    pthread_attr_destroy(&a);
    if (r != 0) { free(t); return false; }
    return true;
}

/* ------------------------------------------------------------------ network */

bool plat_net_init(void)
{
    signal(SIGPIPE, SIG_IGN);
    return true;
}

void plat_net_shutdown(void) {}

/* ---- tests: a fake "Wi-Fi switch". The file named by LS_WIFI_FILE says "off" while Wi-Fi is off; the test script writes it. ---- */
static uint64_t s_wifi_on_at;                          /* when a successful switch-on request takes effect (0 = none pending) */

static bool wifi_fake_off(void)
{
    const char *f = getenv("LS_WIFI_FILE");
    if (!f || !f[0]) return false;
    if (s_wifi_on_at && plat_time_ms() >= s_wifi_on_at) {
        FILE *o = fopen(f, "w");
        if (o) { fputs("on\n", o); fclose(o); }
        s_wifi_on_at = 0;
    }
    FILE *in = fopen(f, "r");
    if (!in) return false;
    char b[8] = "";
    if (!fgets(b, sizeof b, in)) b[0] = 0;
    fclose(in);
    return strncmp(b, "off", 3) == 0;
}

int plat_wifi_turn_on(char *detail, size_t cap)
{
    const char *mode = getenv("LS_WIFI_TURNON");
    if (!mode) mode = "ok";
    if (!strcmp(mode, "fail")) { snprintf(detail, cap, "not supported (test)"); return -1; }
    if (!strcmp(mode, "ok")) {
        const char *d = getenv("LS_WIFI_DELAY_MS");
        s_wifi_on_at = plat_time_ms() + (d ? (uint64_t)atoi(d) : 600u);
        if (s_wifi_on_at == 0) s_wifi_on_at = 1;
    }
    snprintf(detail, cap, "test (%s)", mode);          /* "nojoin": accepted, but the network never comes */
    return 0;
}

bool plat_open_wifi_settings(const char *uri)
{
    if (getenv("LS_WIFI_SETTINGS_FAIL")) { ls_log("Wi-Fi: could not open the Wi-Fi settings (test)"); return false; }
    ls_log("Wi-Fi: opening the Wi-Fi settings (test: the app keeps running; address %s)", uri && uri[0] ? uri : "default");
    return true;
}

bool plat_net_local_ip(char out[16])
{
    if (wifi_fake_off()) { out[0] = 0; return false; }
    /* "connect" a UDP socket to a public address (nothing is sent) to learn our outgoing IP */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);
    bool ok = false;
    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) {
        struct sockaddr_in me;
        socklen_t l = sizeof me;
        if (getsockname(s, (struct sockaddr *)&me, &l) == 0) {
            inet_ntop(AF_INET, &me.sin_addr, out, 16);
            ok = true;
        }
    }
    close(s);
    if (!ok) snprintf(out, 16, "127.0.0.1");
    return true;
}

static void set_nonblock(int fd, bool on)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

static char s_net_err[96];

const char *plat_net_error_text(void) { return s_net_err[0] ? s_net_err : "no error recorded"; }

static void note_err(const char *stage)
{
    snprintf(s_net_err, sizeof s_net_err, "%s: %s", stage, strerror(errno));
}

ls_sock plat_udp_open(uint16_t port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { note_err("socket"); return LS_BAD_SOCK; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
        note_err("bind");
        close(s);
        return LS_BAD_SOCK;
    }
    set_nonblock(s, true);
    return s;
}

bool plat_udp_join_group(ls_sock s, const char *group, const char *local_ip)
{
    struct ip_mreq m;
    memset(&m, 0, sizeof m);
    inet_pton(AF_INET, group, &m.imr_multiaddr);
    if (local_ip) inet_pton(AF_INET, local_ip, &m.imr_interface);
    else m.imr_interface.s_addr = htonl(INADDR_ANY);
    return setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0;
}

int plat_udp_recvfrom(ls_sock s, void *buf, size_t cap, ls_endpoint *from)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    ssize_t r = recvfrom(s, buf, cap, 0, (struct sockaddr *)&a, &l);
    if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? LS_IO_TIMEOUT : LS_IO_ERROR;
    if (from) {
        inet_ntop(AF_INET, &a.sin_addr, from->ip, sizeof from->ip);
        from->port = ntohs(a.sin_port);
    }
    return (int)r;
}

int plat_udp_sendto(ls_sock s, const void *buf, size_t len, const char *ip, uint16_t port)
{
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) return LS_IO_ERROR;
    ssize_t r = sendto(s, buf, len, 0, (struct sockaddr *)&a, sizeof a);
    return r < 0 ? LS_IO_ERROR : (int)r;
}

ls_sock plat_tcp_listen(uint16_t port, int backlog)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { note_err("socket"); return LS_BAD_SOCK; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { note_err("bind"); close(s); return LS_BAD_SOCK; }
    if (listen(s, backlog) != 0) { note_err("listen"); close(s); return LS_BAD_SOCK; }
    set_nonblock(s, true);
    return s;
}

uint16_t plat_sock_local_port(ls_sock s)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(s, (struct sockaddr *)&a, &l) != 0) return 0;
    return ntohs(a.sin_port);
}

ls_sock plat_tcp_accept(ls_sock listener, ls_endpoint *peer)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int c = accept(listener, (struct sockaddr *)&a, &l);
    if (c < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? LS_IO_TIMEOUT : LS_IO_ERROR;
    set_nonblock(c, false);
    if (peer) {
        inet_ntop(AF_INET, &a.sin_addr, peer->ip, sizeof peer->ip);
        peer->port = ntohs(a.sin_port);
    }
    return c;
}

void plat_sock_set_timeout(ls_sock s, int ms)
{
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

bool plat_resolve(const char *host, char ip[16], int timeout_ms)
{
    (void)timeout_ms;
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) { snprintf(ip, 16, "%s", host); return true; }
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    bool ok = inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, 16) != NULL;
    freeaddrinfo(res);
    return ok;
}

ls_sock plat_tcp_connect(const char *ip, uint16_t port, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return LS_BAD_SOCK;
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); return LS_BAD_SOCK; }

    set_nonblock(s, true);
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r != 0 && errno == EINPROGRESS) {
        struct pollfd p = { s, POLLOUT, 0 };
        if (poll(&p, 1, timeout_ms) == 1) {
            int err = 0;
            socklen_t l = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
            r = err == 0 ? 0 : -1;
        } else {
            r = -1;
        }
    }
    if (r != 0) { note_err("connect"); close(s); return LS_BAD_SOCK; }
    set_nonblock(s, false);
    plat_sock_set_timeout(s, timeout_ms);
    return s;
}

int plat_sock_recv(ls_sock s, void *buf, size_t cap)
{
    for (;;) {
        ssize_t r = recv(s, buf, cap, 0);
        if (r >= 0) return (int)r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return LS_IO_TIMEOUT;
        return LS_IO_ERROR;
    }
}

int plat_sock_send(ls_sock s, const void *buf, size_t len)
{
    for (;;) {
        ssize_t r = send(s, buf, len, MSG_NOSIGNAL);
        if (r >= 0) return (int)r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return LS_IO_TIMEOUT;
        return LS_IO_ERROR;
    }
}

void plat_sock_close(ls_sock s)
{
    if (s >= 0) close(s);
}
