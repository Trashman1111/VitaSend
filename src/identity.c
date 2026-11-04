/*
 * identity.c - This device's identity (alias, model, type, fingerprint, port) and the JSON the protocol wants it in.
 *
 * Author: trashman (VitaSend)
 */
#include "identity.h"
#include "plat.h"
#include "settings.h"
#include "tls.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ls_identity g_id;

static plat_mutex *s_id_lock;

bool identity_load(const char *dir, uint16_t port_override)
{
    memset(&g_id, 0, sizeof g_id);
    if (!s_id_lock) s_id_lock = plat_mutex_create();
    ls_strlcpy(g_id.device_model, "PlayStation Vita", sizeof g_id.device_model);
    ls_strlcpy(g_id.device_type, "mobile", sizeof g_id.device_type);
    ls_strlcpy(g_id.fingerprint, tls_fingerprint(), sizeof g_id.fingerprint);
    g_id.https = true;
    g_id.download = false;

    settings_load(dir);                               /* config.json: alias, port, ... */
    ls_settings st;
    settings_snapshot(&st);
    g_id.port = st.port;
    if (st.alias[0]) {
        ls_strlcpy(g_id.alias, st.alias, sizeof g_id.alias);
    } else {
        /* e.g. "Vita-4F2A" - unique enough to tell several Vitas apart */
        uint8_t r[2];
        tls_random(NULL, r, sizeof r);
        snprintf(g_id.alias, sizeof g_id.alias, "Vita-%02X%02X", r[0], r[1]);
        settings_set_alias(g_id.alias);               /* also writes config.json */
    }
    if (port_override) g_id.port = port_override;
    return true;
}

void identity_set_alias(const char *alias)
{
    plat_mutex_lock(s_id_lock);
    ls_strlcpy(g_id.alias, alias, sizeof g_id.alias);
    plat_mutex_unlock(s_id_lock);
    settings_set_alias(alias);
}

void identity_json(jw *w, bool with_net, int announce_mode)
{
    char alias[sizeof g_id.alias];
    plat_mutex_lock(s_id_lock);
    ls_strlcpy(alias, g_id.alias, sizeof alias);
    plat_mutex_unlock(s_id_lock);
    jw_obj(w);
    jw_kstr(w, "alias", alias);
    jw_kstr(w, "version", LS_PROTO_VERSION);
    jw_kstr(w, "deviceModel", g_id.device_model);
    jw_kstr(w, "deviceType", g_id.device_type);
    jw_kstr(w, "fingerprint", g_id.fingerprint);
    if (with_net) {
        jw_kint(w, "port", g_id.port);
        jw_kstr(w, "protocol", g_id.https ? "https" : "http");
    }
    jw_kbool(w, "download", g_id.download);
    if (announce_mode) {
        /* v2 peers read "announce", v1 peers read "announcement" - send both. */
        jw_kbool(w, "announce", announce_mode == 1);
        jw_kbool(w, "announcement", announce_mode == 1);
    }
    jw_end_obj(w);
}
