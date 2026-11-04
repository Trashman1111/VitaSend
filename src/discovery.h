/*
 * discovery.h - LocalSend multicast discovery (protocol section 3.1).
 *
 *  1. We announce ourselves on 224.0.0.167:53317 (UDP) with "announce": true.
 *  2. Every device that hears it answers with  POST /api/localsend/v2/register  to our HTTPS server.
 *  3. We hear *their* announcements too, and answer them the same way (HTTP register to their server;
 *     if that fails, a UDP reply with "announce": false).
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_DISCOVERY_H
#define LS_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where announcements are sent (default: 224.0.0.167 : 53317) and which UDP port we listen on. */
void discovery_configure(const char *announce_ip, uint16_t announce_port, uint16_t listen_port);
bool discovery_start(void);
void discovery_request_announce(void);   /* "I'm here" burst right now (e.g. user pressed a button) */
void discovery_network_changed(void);    /* Wi-Fi (re)connected: reopen the socket and announce */
bool discovery_listening(void);          /* true if the UDP port was bound */

/* "Add by address": contacts ip:port (https, then http), registers with it and adds it to the peer list.
 * Runs in the background; the outcome is picked up with discovery_take_manual_result(). */
bool discovery_add_manual(const char *ip, uint16_t port);
bool discovery_take_manual_result(char *out, size_t cap);

#endif
