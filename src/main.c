/*
 * main.c - program start and shutdown.
 *
 *   1. platform + screen + log file
 *   2. self tests (random numbers, SHA-256, device certificate, an in-memory TLS handshake) - shown on a plain boot screen
 *   3. device identity (alias, fingerprint), transfer / history / peer state
 *   4. wait for Wi-Fi, start the HTTPS server (port 53317, or the next free one) and the discovery thread
 *   5. app_run() - the user interface (app.c); returns when the user quits
 *   6. stop the threads, release everything
 *
 * Build with -DLS_HELLO_ONLY (cmake -DHELLO_ONLY=ON) to stop after step 2: a quick check that the toolchain, mbedTLS and
 * the entropy source work. The PC test build (host/) accepts extra command line options, see the usage text below.
 *
 * Author: trashman (VitaSend)
 */
#include "app.h"
#include "discovery.h"
#include "history.h"
#include "http.h"
#include "identity.h"
#include "peers.h"
#include "plat.h"
#include "rx.h"
#include "tls.h"
#include "util.h"
#include "ui.h"
#include "vpk.h"
#include "xfer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __vita__
/* heap for malloc(): the default is small */
int _newlib_heap_size_user = 64 * 1024 * 1024;
#endif

#ifdef LS_HOST
void ui_host_set_dump(const char *path);
void ui_host_set_ppm(const char *path);
void ui_host_set_keys(const char *script);
void ui_host_set_key_fifo(const char *path);
#endif

/* The plain screen used while starting up (and when something is badly wrong). */
static char        s_status[160];
static gcol        s_status_color = C_WARN;
static const char *s_footer = "";

static void redraw(void)
{
    ui_boot_screen(s_status, s_status_color, s_footer);
}

static void set_status(gcol color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_status(gcol color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof s_status, fmt, ap);
    va_end(ap);
    s_status_color = color;
}

/* ------------------------------------------------------------------ self tests */

static bool self_tests(const char *dir)
{
    ls_log("%s", tls_version_string());

    if (!tls_init_rng()) {
        ls_log("FAIL entropy / random number generator");
        return false;
    }
    ls_log("OK   random numbers (hardware entropy + CTR-DRBG)");
    redraw();

    if (!tls_selftest_sha256()) {
        ls_log("FAIL SHA-256 test vector");
        return false;
    }
    ls_log("OK   SHA-256 test vector");
    redraw();

    uint64_t t0 = plat_time_ms();
    if (!tls_init_identity(dir)) {
        ls_log("FAIL device certificate");
        return false;
    }
    ls_log("OK   device certificate ready (%llu ms)", (unsigned long long)(plat_time_ms() - t0));
    redraw();

    double ms = 0;
    if (!tls_selftest_loopback(&ms)) {
        ls_log("FAIL TLS 1.2 handshake in memory");
        return false;
    }
    ls_log("OK   TLS 1.2 handshake in memory: %.0f ms", ms);
    return true;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    ls_util_init();

    const char *announce_ip = NULL;
    uint16_t    announce_port = 0, port_override = 0;
    unsigned    run_seconds = 0;
#ifdef LS_HOST
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--data") && i + 1 < argc) plat_posix_set_data_dir(argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port_override = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) run_seconds = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-screen") && i + 1 < argc) ui_host_set_dump(argv[++i]);
        else if (!strcmp(argv[i], "--dump-ppm") && i + 1 < argc) ui_host_set_ppm(argv[++i]);
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) ui_host_set_keys(argv[++i]);
        else if (!strcmp(argv[i], "--keys-fifo") && i + 1 < argc) ui_host_set_key_fifo(argv[++i]);
        else if (!strcmp(argv[i], "--browse-start") && i + 1 < argc) plat_posix_set_browse_start(argv[++i]);
        else if (!strcmp(argv[i], "--free-space") && i + 1 < argc) plat_posix_set_free_space((uint64_t)strtoull(argv[++i], NULL, 10));
        else if (!strcmp(argv[i], "--announce-to") && i + 1 < argc) {
            static char ipbuf[32];
            snprintf(ipbuf, sizeof ipbuf, "%s", argv[++i]);
            char *colon = strchr(ipbuf, ':');
            if (colon) { *colon = 0; announce_port = (uint16_t)atoi(colon + 1); }
            announce_ip = ipbuf;
        } else {
            printf("usage: %s [--data DIR] [--port N] [--announce-to IP:PORT] [--seconds N] [--dump-screen FILE]\n"
                   "          [--keys \"cross,down,wait500,...\"] [--keys-fifo PATH] [--browse-start DIR]\n", argv[0]);
            return 2;
        }
    }
#else
    (void)argc;
    (void)argv;
#endif

    plat_init();
    const char *dir = plat_data_dir();
    plat_mkdirs(dir);
    char logpath[300];
    ls_path_join(logpath, sizeof logpath, dir, "log.txt");
    ls_log_set_file(logpath);

    ui_init();
    if (!gfx_init()) ls_log("FAIL not enough memory for the screen");
    ls_log("%s %s starting", LS_APP_NAME, LS_APP_VERSION);

    s_footer = "START Quit";
    set_status(C_WARN, "Running self tests...");
    redraw();

    bool ok = self_tests(dir);
    if (!ok) {
        set_status(C_BAD, "Self test FAILED - see the log below and log.txt on the memory card");
        s_footer = "START Quit";
        for (;;) {
            redraw();
            if (ui_buttons() & UI_BTN_START) break;
            plat_sleep_ms(100);
            if (run_seconds) break;
        }
        ui_shutdown();
        return 1;
    }

    identity_load(dir, port_override);               /* alias + fingerprint, needed on screen from here on */

#ifdef LS_HELLO_ONLY
    (void)announce_ip;
    (void)announce_port;
    set_status(C_GOOD, "Hello from the Vita! mbedTLS works. Phase 1 complete.");
    s_footer = "START Quit";
    {
        uint64_t t0 = plat_time_ms();
        for (;;) {
            redraw();
            if (ui_buttons() & UI_BTN_START) break;
            plat_keep_awake();
            plat_sleep_ms(100);
            if (run_seconds && plat_time_ms() - t0 > run_seconds * 1000u) break;
        }
    }
    tls_shutdown();
    ui_shutdown();
    return 0;
#else
    /* ---------------- network, server, discovery ---------------- */
    xfer_init();
    rx_init();
    vpk_init();
    history_load(dir);
    peers_init();
    ls_log("device name: %s", g_id.alias);

    if (!plat_net_init()) {
        ls_log("FAIL network init: %s", plat_net_error_text());
        set_status(C_BAD, "Network init failed");
    }

    /* wait for Wi-Fi */
    char ip[16] = "";
    s_footer = "START Quit";
    uint32_t prev_buttons = ui_buttons();
    uint64_t t_start = plat_time_ms();
    while (!g_quit && !plat_net_local_ip(ip)) {
        set_status(C_WARN, "Waiting for Wi-Fi (turn it on in the Vita settings)...");
        redraw();
        uint32_t b = ui_buttons();
        if ((b & UI_BTN_START) && !(prev_buttons & UI_BTN_START)) g_quit = true;
        prev_buttons = b;
        plat_keep_awake();
        plat_sleep_ms(250);
        if (run_seconds && plat_time_ms() - t_start > run_seconds * 1000u) g_quit = true;
    }
    if (!g_quit) ls_log("Wi-Fi connected, IP %s", ip);

    bool server_ok = false;
    if (!g_quit) {
        /* If the usual port is taken (for example by a copy of this app that did not shut down properly), use the next
         * free one: other devices learn our port from the announcement, so this still works. */
        uint16_t wanted_port = g_id.port;
        for (unsigned i = 0; i < 11 && !server_ok; i++) {
            uint16_t p = i < 10 ? (uint16_t)(wanted_port + i) : 0;       /* last resort: let the system pick a port */
            server_ok = http_server_start(&p, g_id.https);
            if (server_ok) {
                g_id.port = p;
                if (i == 0) ls_log("HTTPS server listening on port %u", p);
                else ls_log("HTTPS server listening on port %u (%u was not available)", p, wanted_port);
            } else if (i == 0) {
                ls_log("FAIL TCP port %u: %s", wanted_port, plat_net_error_text());
            }
        }
        if (!server_ok) ls_log("FAIL no TCP port could be opened (tried %u-%u and any free port)", wanted_port, wanted_port + 9u);

        discovery_configure(announce_ip, announce_port, wanted_port);
        if (!discovery_start()) ls_log("FAIL could not start the discovery thread");
    }

    if (!g_quit) {
        app_opts ao = { run_seconds, server_ok };
        app_run(&ao);
    }

    ls_log("shutting down...");
    g_quit = true;
    xfer_request_cancel();
    s_footer = "";
    set_status(C_DIM, "Shutting down...");
    redraw();
    ls_wait_threads(4000);
    plat_net_shutdown();
    tls_shutdown();
    gfx_shutdown();
    ui_shutdown();
    return 0;
#endif
}
