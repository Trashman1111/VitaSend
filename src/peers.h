/*
 * peers.h - the list of other LocalSend devices we have seen (thread safe).
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_PEERS_H
#define LS_PEERS_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_PEERS 48

typedef struct {
    char     alias[64];
    char     model[32];
    char     type[16];
    char     fingerprint[65];
    char     ip[16];
    uint16_t port;
    bool     https;
    bool     download;
    uint64_t last_seen_ms;
    uint64_t last_register_ms;    /* when we last sent /register to it (0 = never) */
} ls_peer;

void peers_init(void);
/* Insert or update (key = fingerprint). Returns true if the peer is new. */
bool peers_upsert(const ls_peer *p);
/* Copies up to max peers (newest first). Returns count. */
int  peers_snapshot(ls_peer *out, int max);
int  peers_count(void);
/* Most recently seen peer with this IP address (false if none). */
bool peers_find_by_ip(const char *ip, ls_peer *out);
/* True if we have not sent /register to this peer in the last min_gap_ms; marks it as sent now. */
bool peers_claim_register(const char *fingerprint, uint64_t min_gap_ms);

#endif
