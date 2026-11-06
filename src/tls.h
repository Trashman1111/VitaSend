/*
 * tls.h - everything that touches mbedTLS.
 *
 *  - random numbers (thread safe)
 *  - our device identity: a self-signed ECDSA P-256 certificate, stored on the memory card,
 *    whose SHA-256 is the LocalSend "fingerprint"
 *  - conn_t: one connection, either HTTPS (TLS 1.2) or plain HTTP, server or client side
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_TLS_H
#define LS_TLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "plat.h"

/* Step 1 (Phase 1): entropy + DRBG. Must succeed before anything else. */
bool tls_init_rng(void);
int  tls_random(void *unused, unsigned char *out, size_t len);   /* mbedTLS f_rng compatible, thread safe */

/* Step 2: load key+cert from dir (or generate and save them), build the TLS configs. */
bool        tls_init_identity(const char *dir);
const char *tls_fingerprint(void);            /* 64 lowercase hex chars */
double      tls_last_keygen_ms(void);         /* 0 if the key was loaded, not generated */

/* Phase-1 self tests (each returns true on success and logs details). */
const char *tls_version_string(void);
bool        tls_selftest_sha256(void);
bool        tls_selftest_loopback(double *handshake_ms);   /* full TLS 1.2 handshake in memory */

void tls_shutdown(void);

/* ---------------------------------------------------------------- SHA-256 (incremental) */
typedef struct ls_sha256 ls_sha256;
ls_sha256 *sha256_new(void);
void       sha256_update(ls_sha256 *h, const void *data, size_t len);
void       sha256_finish_hex(ls_sha256 *h, char out[65]);   /* also frees h */
void       sha256_discard(ls_sha256 *h);

/* ---------------------------------------------------------------- connections */
typedef struct conn conn_t;

/* Takes ownership of an accepted socket. For TLS this performs the server handshake.
 * Returns NULL (socket closed) on failure. */
conn_t *conn_accept(ls_sock fd, bool use_tls, int timeout_ms);

/* Connects to ip:port (and does the client handshake for TLS). NULL on failure. */
conn_t *conn_connect(const char *ip, uint16_t port, bool use_tls, int timeout_ms);

/* >0 bytes read, 0 = peer closed, LS_IO_TIMEOUT, LS_IO_ERROR */
int  conn_read(conn_t *c, void *buf, size_t cap);
/* 0 = everything written, <0 = error */
int  conn_write_all(conn_t *c, const void *buf, size_t len);
/* Fingerprint of the certificate the other side presented (client side). */
bool conn_peer_fingerprint(conn_t *c, char out[65]);
/* Connects to ip:port and checks the server: its certificate must chain to a trusted root (the built-in list, cacerts.c) and
 * carry the name `host`, which is also sent as the server name. For the update check (GitHub); LocalSend peers use
 * conn_connect() and the fingerprint. NULL on failure, `why` then holds a short readable reason. (Certificate dates are not
 * checked: the Vita's clock is often wrong and this mbedTLS build has no time support.) */
conn_t *conn_connect_verified(const char *ip, uint16_t port, const char *host, int timeout_ms, char *why, size_t whycap);
int  tls_roots_usable(void);                  /* how many of the built-in root certificates can be read */
void conn_set_timeout(conn_t *c, int ms);
void conn_close(conn_t *c);

#endif
