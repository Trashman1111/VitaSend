/*
 * wifi.h - "Wi-Fi is off": asking the system to switch it on, and following what happens.
 *
 * One small state machine shared by the start-up dialog (main.c) and the Receive page / sidebar (app*.c, ui.c). It does not
 * decide whether Wi-Fi is on - plat_net_local_ip() does - it only remembers that we asked, and for how long we wait.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_WIFI_H
#define LS_WIFI_H

#include <stdbool.h>

typedef enum {
    WIFI_IDLE,         /* nothing asked (Wi-Fi is on, or off and the user has not pressed the button yet) */
    WIFI_CONNECTING,   /* we asked the system to switch it on and are waiting for an address */
    WIFI_FAILED        /* the system refused, or no address came in time: the user has to use the Vita settings */
} wifi_state;

wifi_state wifi_get(void);
/* The user asked for Wi-Fi: send the request (once; ignored while one is running). */
void wifi_request_on(void);
/* What a Wi-Fi button does: asks the system to switch Wi-Fi on; once that was refused, it opens the Vita's Wi-Fi settings instead
 * (and says so in the log). */
unsigned wifi_grace_ms(void);               /* at start: how long to wait for Wi-Fi to come up by itself before asking (it needs a few seconds after waking from sleep) */
void wifi_user_action(void);
/* The user gave up waiting. */
void wifi_cancel(void);
/* Call regularly with whether an IP address exists now (false = no Wi-Fi). Moves CONNECTING to IDLE when it came up and to
 * FAILED when the time ran out. Returns true if the state changed. */
bool wifi_poll(bool has_ip);
/* Why the last request failed ("" if it did not), for the dialog's log line. */
const char *wifi_fail_reason(void);

#endif
