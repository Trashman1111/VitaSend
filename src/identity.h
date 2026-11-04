/*
 * identity.h - who we are on the network (alias, device type, port, fingerprint).
 * The alias lives in  <data dir>/config.json  (change it in Settings, or edit the file).
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_IDENTITY_H
#define LS_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#include "json.h"

typedef struct {
    char     alias[64];
    char     device_model[32];     /* "PlayStation Vita"               */
    char     device_type[16];      /* mobile | desktop | web | headless | server */
    char     fingerprint[65];      /* sha256 of our certificate, hex   */
    uint16_t port;
    bool     https;
    bool     download;             /* download API (browser URL) - not supported */
} ls_identity;

extern ls_identity g_id;

/* Reads/creates config.json in dir. Call after tls_init_identity(). */
bool identity_load(const char *dir, uint16_t port_override);

/* Changes the device name everywhere (announcements, config.json). */
void identity_set_alias(const char *alias);

/* Writes our info as a JSON object.
 *   with_net     : include "port" and "protocol"
 *   announce_mode: 0 = no announce fields, 1 = "announce":true, 2 = "announce":false */
void identity_json(jw *w, bool with_net, int announce_mode);

#endif
