/*
 * app.h - the user interface: home screen, file browser, device picker, settings, history,
 * incoming-request dialog and transfer progress. Runs the main loop.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_APP_H
#define LS_APP_H

#include <stdbool.h>

typedef struct {
    unsigned run_seconds;       /* host test builds: stop after this long (0 = run until the user quits) */
    bool     server_ok;         /* the HTTPS server is listening */
    bool   (*start_services)(void);   /* started without Wi-Fi: called once when Wi-Fi comes; starts server + discovery, returns server_ok (NULL = already running) */
} app_opts;

/* Returns when the user quits (or run_seconds elapsed, or g_quit was set). */
void app_run(const app_opts *opts);

#endif
