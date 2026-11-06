/*
 * wifi.c - see wifi.h
 *
 * Author: trashman (VitaSend)
 */
#include "wifi.h"

#include "plat.h"
#include "util.h"
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>

#define WIFI_WAIT_MS 20000u      /* how long to wait for an address after the system accepted the request */

static wifi_state s_state = WIFI_IDLE;
static uint64_t   s_deadline;
static char       s_reason[96];

wifi_state wifi_get(void) { return s_state; }

const char *wifi_fail_reason(void) { return s_reason; }

static unsigned wait_ms(void)
{
#ifdef LS_HOST
    const char *e = getenv("LS_WIFI_TIMEOUT_MS");          /* tests: do not wait 20 seconds for a failure */
    if (e && atoi(e) > 0) return (unsigned)atoi(e);
#endif
    return WIFI_WAIT_MS;
}

void wifi_request_on(void)
{
    if (s_state == WIFI_CONNECTING) return;
    char detail[96] = "";
    int r = plat_wifi_turn_on(detail, sizeof detail);
    if (r < 0) {
        ls_strlcpy(s_reason, detail[0] ? detail : "refused", sizeof s_reason);
        ls_log("Wi-Fi: could not be switched on from the app (%s)", s_reason);
        s_state = WIFI_FAILED;
        return;
    }
    s_reason[0] = 0;
    ls_log("Wi-Fi: switch-on requested%s%s", detail[0] ? " - " : "", detail);
    s_deadline = plat_time_ms() + wait_ms();
    s_state = WIFI_CONNECTING;
}

void wifi_user_action(void)
{
    if (s_state == WIFI_FAILED) {
        char uri[96]; settings_wifi_uri(uri, sizeof uri);
        if (!plat_open_wifi_settings(uri)) ls_log("Wi-Fi: the Wi-Fi settings could not be opened from here");
        return;
    }
    wifi_request_on();
}

void wifi_cancel(void)
{
    if (s_state == WIFI_CONNECTING) ls_log("Wi-Fi: waiting cancelled");
    s_state = WIFI_IDLE;
}

bool wifi_poll(bool has_ip)
{
    wifi_state old = s_state;
    if (has_ip) {
        if (s_state != WIFI_IDLE) ls_log("Wi-Fi: connected");
        s_state = WIFI_IDLE;
    } else if (s_state == WIFI_CONNECTING && plat_time_ms() > s_deadline) {
        ls_strlcpy(s_reason, "no network address after the switch-on request", sizeof s_reason);
        ls_log("Wi-Fi: %s", s_reason);
        s_state = WIFI_FAILED;
    }
    return s_state != old;
}
