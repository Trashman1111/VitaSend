/*
 * peers.c - The list of other LocalSend devices we have seen (thread safe): insert / refresh / snapshot for the UI.
 *
 * Author: trashman (VitaSend)
 */
#include "peers.h"
#include "plat.h"
#include "util.h"

#include <string.h>

static plat_mutex *s_lock;
static ls_peer     s_peers[MAX_PEERS];
static int         s_n;

void peers_init(void)
{
    s_lock = plat_mutex_create();
    s_n = 0;
}

static int find(const char *fp)
{
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_peers[i].fingerprint, fp)) return i;
    return -1;
}

bool peers_upsert(const ls_peer *p)
{
    bool is_new = false;
    plat_mutex_lock(s_lock);
    int i = find(p->fingerprint);
    if (i < 0) {
        is_new = true;
        if (s_n < MAX_PEERS) {
            i = s_n++;
        } else {                                   /* full: replace the least recently seen */
            i = 0;
            for (int k = 1; k < s_n; k++)
                if (s_peers[k].last_seen_ms < s_peers[i].last_seen_ms) i = k;
        }
        memset(&s_peers[i], 0, sizeof s_peers[i]);
    }
    uint64_t keep_reg = s_peers[i].last_register_ms;
    s_peers[i] = *p;
    s_peers[i].last_register_ms = keep_reg;
    s_peers[i].last_seen_ms = plat_time_ms();
    plat_mutex_unlock(s_lock);
    return is_new;
}

int peers_snapshot(ls_peer *out, int max)
{
    plat_mutex_lock(s_lock);
    /* newest first */
    int n = 0;
    bool used[MAX_PEERS] = { false };
    while (n < max && n < s_n) {
        int best = -1;
        for (int i = 0; i < s_n; i++)
            if (!used[i] && (best < 0 || s_peers[i].last_seen_ms > s_peers[best].last_seen_ms)) best = i;
        used[best] = true;
        out[n++] = s_peers[best];
    }
    plat_mutex_unlock(s_lock);
    return n;
}

int peers_count(void)
{
    plat_mutex_lock(s_lock);
    int n = s_n;
    plat_mutex_unlock(s_lock);
    return n;
}

bool peers_claim_register(const char *fingerprint, uint64_t min_gap_ms)
{
    bool ok = true;
    plat_mutex_lock(s_lock);
    int i = find(fingerprint);
    if (i >= 0) {
        uint64_t now = plat_time_ms();
        if (s_peers[i].last_register_ms && now - s_peers[i].last_register_ms < min_gap_ms) ok = false;
        else s_peers[i].last_register_ms = now;
    }
    plat_mutex_unlock(s_lock);
    return ok;
}

bool peers_find_by_ip(const char *ip, ls_peer *out)
{
    bool found = false;
    plat_mutex_lock(s_lock);
    int best = -1;
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_peers[i].ip, ip) && (best < 0 || s_peers[i].last_seen_ms > s_peers[best].last_seen_ms)) best = i;
    if (best >= 0) { *out = s_peers[best]; found = true; }
    plat_mutex_unlock(s_lock);
    return found;
}
