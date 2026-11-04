/*
 * tls.c - mbedTLS glue: hardware random numbers, the device certificate (ECDSA P-256, created on first start), TLS server and
 * client connections wrapped as conn_t (so http.c does not care whether TLS is used), SHA-256.
 *
 * Author: trashman (VitaSend)
 */
#include "tls.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR < 3
#error "LocalSend Vita needs mbedTLS 3.x. Run scripts/build-mbedtls.sh (see README) and point MBEDTLS_ROOT at it."
#endif
#include <mbedtls/build_info.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

#define HANDSHAKE_TIMEOUT_MS 8000

/* ================================================================== globals */

static mbedtls_entropy_context  s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static plat_mutex              *s_rng_lock;
static bool                     s_rng_ok;

static uint8_t *s_key_der;      /* our private key  (SEC1/PKCS#8 DER) */
static size_t   s_key_len;
static uint8_t *s_cert_der;     /* our certificate  (X.509 DER)       */
static size_t   s_cert_len;
static char     s_fp[65];
static double   s_keygen_ms;

static void log_mbed(const char *what, int err)
{
    char msg[100];
    mbedtls_strerror(err, msg, sizeof msg);
    ls_log("%s failed: -0x%04X %s", what, (unsigned)-err, msg);
}

/* ================================================================== randomness */

/* Called by mbedTLS (MBEDTLS_ENTROPY_HARDWARE_ALT) - the platform layer provides the bytes.
 * (3.x keeps this prototype in a private header, so it is declared here.) */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    if (!plat_random(output, len)) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = len;
    return 0;
}

bool tls_init_rng(void)
{
    if (s_rng_ok) return true;
    s_rng_lock = plat_mutex_create();

    mbedtls_entropy_init(&s_entropy);
    /* Harmless if the library already registered it through ENTROPY_HARDWARE_ALT. */
    int r = mbedtls_entropy_add_source(&s_entropy, mbedtls_hardware_poll, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
    if (r != 0) {
        log_mbed("entropy_add_source", r);
        return false;
    }
    mbedtls_ctr_drbg_init(&s_drbg);
    r = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                              (const unsigned char *)"localsend-vita", 14);
    if (r != 0) {
        log_mbed("ctr_drbg_seed", r);
        return false;
    }
#if defined(MBEDTLS_PSA_CRYPTO_C)
    if (psa_crypto_init() != PSA_SUCCESS)
        ls_log("note: psa_crypto_init failed (not needed for TLS 1.2)");
#endif
    s_rng_ok = true;
    return true;
}

int tls_random(void *unused, unsigned char *out, size_t len)
{
    (void)unused;
    if (!s_rng_ok) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    plat_mutex_lock(s_rng_lock);
    int r = mbedtls_ctr_drbg_random(&s_drbg, out, len);
    plat_mutex_unlock(s_rng_lock);
    return r;
}

/* ================================================================== identity */

static bool sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    uint8_t h[32];
    if (mbedtls_sha256(data, len, h, 0) != 0) return false;
    ls_hex(h, 32, out);
    return true;
}

static bool pair_ok(const uint8_t *key_der, size_t key_len, const uint8_t *cert_der, size_t cert_len)
{
    mbedtls_pk_context key;
    mbedtls_x509_crt   crt;
    mbedtls_pk_init(&key);
    mbedtls_x509_crt_init(&crt);
    bool ok = false;
    int r = mbedtls_pk_parse_key(&key, key_der, key_len, NULL, 0, tls_random, NULL);
    if (r != 0) { log_mbed("parse key", r); goto done; }
    r = mbedtls_x509_crt_parse_der(&crt, cert_der, cert_len);
    if (r != 0) { log_mbed("parse certificate", r); goto done; }
    r = mbedtls_pk_check_pair(&crt.pk, &key, tls_random, NULL);
    if (r != 0) { log_mbed("key/certificate mismatch", r); goto done; }
    ok = true;
done:
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&crt);
    return ok;
}

static bool generate_identity(uint8_t **key_der, size_t *key_len, uint8_t **cert_der, size_t *cert_len)
{
    mbedtls_pk_context     key;
    mbedtls_x509write_cert w;
    unsigned char          kbuf[512], cbuf[1024], serial[16];
    bool                   ok = false;
    int                    r;

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&w);

    uint64_t t0 = plat_time_ms();
    if ((r = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0) { log_mbed("pk_setup", r); goto done; }
    if ((r = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), tls_random, NULL)) != 0) {
        log_mbed("ecp_gen_key", r);
        goto done;
    }
    s_keygen_ms = (double)(plat_time_ms() - t0);

    const char *dn = "CN=LocalSend User,O=LocalSend,OU=PS Vita";
    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &key);
    mbedtls_x509write_crt_set_issuer_key(&w, &key);
    if ((r = mbedtls_x509write_crt_set_subject_name(&w, dn)) != 0) { log_mbed("subject name", r); goto done; }
    if ((r = mbedtls_x509write_crt_set_issuer_name(&w, dn)) != 0) { log_mbed("issuer name", r); goto done; }
    tls_random(NULL, serial, sizeof serial);
    serial[0] = (unsigned char)((serial[0] & 0x7f) | 0x01);              /* positive, non-zero */
    if ((r = mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial)) != 0) { log_mbed("serial", r); goto done; }
    /* The Vita clock is often wrong, so use a wide fixed validity window. */
    if ((r = mbedtls_x509write_crt_set_validity(&w, "20200101000000", "20491231235959")) != 0) { log_mbed("validity", r); goto done; }

    int cl = mbedtls_x509write_crt_der(&w, cbuf, sizeof cbuf, tls_random, NULL);
    if (cl <= 0) { log_mbed("crt_der", cl); goto done; }
    int kl = mbedtls_pk_write_key_der(&key, kbuf, sizeof kbuf);
    if (kl <= 0) { log_mbed("write_key_der", kl); goto done; }

    /* mbedTLS writes DER at the END of the buffer */
    *cert_der = malloc((size_t)cl);
    *key_der  = malloc((size_t)kl);
    if (!*cert_der || !*key_der) { free(*cert_der); free(*key_der); *cert_der = *key_der = NULL; goto done; }
    memcpy(*cert_der, cbuf + sizeof cbuf - (size_t)cl, (size_t)cl);
    memcpy(*key_der, kbuf + sizeof kbuf - (size_t)kl, (size_t)kl);
    *cert_len = (size_t)cl;
    *key_len  = (size_t)kl;
    ok = true;
done:
    mbedtls_x509write_crt_free(&w);
    mbedtls_pk_free(&key);
    mbedtls_platform_zeroize(kbuf, sizeof kbuf);
    return ok;
}

bool tls_init_identity(const char *dir)
{
    char kpath[300], cpath[300];
    ls_path_join(kpath, sizeof kpath, dir, "key.der");
    ls_path_join(cpath, sizeof cpath, dir, "cert.der");

    uint8_t *k = NULL, *c = NULL;
    size_t   kl = 0, cl = 0;
    bool     loaded = false;

    if (ls_file_read(kpath, &k, &kl) && ls_file_read(cpath, &c, &cl)) {
        if (pair_ok(k, kl, c, cl)) {
            loaded = true;
            ls_log("loaded device certificate from %s", dir);
        } else {
            ls_log("stored certificate is unusable - generating a new one");
        }
    }
    if (!loaded) {
        free(k);
        free(c);
        k = c = NULL;
        ls_log("generating device certificate (ECDSA P-256)...");
        if (!generate_identity(&k, &kl, &c, &cl)) return false;
        if (!pair_ok(k, kl, c, cl)) { free(k); free(c); return false; }
        if (!ls_file_write(kpath, k, kl) || !ls_file_write(cpath, c, cl))
            ls_log("warning: could not save the certificate in %s (new fingerprint every start)", dir);
        else
            ls_log("certificate created and saved (key generation took %.0f ms)", s_keygen_ms);
    }

    free(s_key_der);
    free(s_cert_der);
    s_key_der = k; s_key_len = kl;
    s_cert_der = c; s_cert_len = cl;
    if (!sha256_hex(s_cert_der, s_cert_len, s_fp)) return false;
    return true;
}

const char *tls_fingerprint(void)    { return s_fp; }
double      tls_last_keygen_ms(void) { return s_keygen_ms; }

void tls_shutdown(void)
{
    free(s_key_der);
    free(s_cert_der);
    s_key_der = s_cert_der = NULL;
    if (s_rng_ok) {
        mbedtls_ctr_drbg_free(&s_drbg);
        mbedtls_entropy_free(&s_entropy);
        s_rng_ok = false;
    }
}

/* ================================================================== per-connection TLS state
 *
 * Every connection gets its own config + copy of the key/cert. That means connections never
 * share mutable mbedTLS state, so handshakes can safely run in parallel on different threads.
 */
typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_x509_crt    crt;
    mbedtls_pk_context  key;
} tls_state;

static void tls_state_free(tls_state *t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->crt);
    mbedtls_pk_free(&t->key);
}

static bool tls_state_init(tls_state *t, bool server)
{
    int r;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->crt);
    mbedtls_pk_init(&t->key);

    if (!s_cert_der || !s_key_der) { ls_log("TLS identity not initialised"); return false; }
    if ((r = mbedtls_x509_crt_parse_der(&t->crt, s_cert_der, s_cert_len)) != 0) { log_mbed("parse cert", r); goto fail; }
    if ((r = mbedtls_pk_parse_key(&t->key, s_key_der, s_key_len, NULL, 0, tls_random, NULL)) != 0) { log_mbed("parse key", r); goto fail; }
    if ((r = mbedtls_ssl_config_defaults(&t->conf, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        log_mbed("ssl_config_defaults", r);
        goto fail;
    }
    mbedtls_ssl_conf_rng(&t->conf, tls_random, NULL);
    /* TLS 1.2 only: every LocalSend implementation supports it, and it keeps memory/CPU low. */
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    /* LocalSend uses self-signed certificates and trusts by fingerprint, so nothing is verified here. */
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    /* As a client we also present our certificate in case the server asks for one. */
    if ((r = mbedtls_ssl_conf_own_cert(&t->conf, &t->crt, &t->key)) != 0) { log_mbed("own_cert", r); goto fail; }
    if ((r = mbedtls_ssl_setup(&t->ssl, &t->conf)) != 0) { log_mbed("ssl_setup", r); goto fail; }
    return true;
fail:
    tls_state_free(t);
    return false;
}

/* ================================================================== sockets <-> mbedTLS */

struct conn {
    ls_sock   fd;
    bool      tls;
    int       timeout_ms;
    tls_state *st;
    char      peer_fp[65];
};

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int r = plat_sock_send(*(ls_sock *)ctx, buf, len);
    if (r > 0) return r;
    if (r == LS_IO_TIMEOUT) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int r = plat_sock_recv(*(ls_sock *)ctx, buf, len);
    if (r > 0) return r;
    if (r == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    if (r == LS_IO_TIMEOUT) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static bool handshake(conn_t *c, bool server)
{
    uint64_t deadline = plat_time_ms() + HANDSHAKE_TIMEOUT_MS;
    int r;
    mbedtls_ssl_set_bio(&c->st->ssl, &c->fd, bio_send, bio_recv, NULL);
    while ((r = mbedtls_ssl_handshake(&c->st->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (plat_time_ms() > deadline) {
                ls_log("TLS handshake timed out");
                return false;
            }
            continue;
        }
        if (r != MBEDTLS_ERR_SSL_CONN_EOF)
            log_mbed(server ? "TLS handshake (server)" : "TLS handshake (client)", r);
        else
            ls_log("TLS handshake: peer closed the connection");
        return false;
    }
    const mbedtls_x509_crt *pc = mbedtls_ssl_get_peer_cert(&c->st->ssl);
    if (pc && pc->raw.p) sha256_hex(pc->raw.p, pc->raw.len, c->peer_fp);
    return true;
}

static conn_t *conn_new(ls_sock fd, bool use_tls, int timeout_ms)
{
    conn_t *c = calloc(1, sizeof *c);
    if (!c) { plat_sock_close(fd); return NULL; }
    c->fd = fd;
    c->tls = use_tls;
    c->timeout_ms = timeout_ms;
    return c;
}

static conn_t *conn_start_tls(conn_t *c, bool server)
{
    c->st = calloc(1, sizeof *c->st);
    if (!c->st || !tls_state_init(c->st, server)) {
        free(c->st);
        c->st = NULL;
        conn_close(c);
        return NULL;
    }
    plat_sock_set_timeout(c->fd, 1000);               /* short reads while handshaking */
    if (!handshake(c, server)) {
        tls_state_free(c->st);
        free(c->st);
        c->st = NULL;
        conn_close(c);
        return NULL;
    }
    plat_sock_set_timeout(c->fd, c->timeout_ms);
    return c;
}

conn_t *conn_accept(ls_sock fd, bool use_tls, int timeout_ms)
{
    conn_t *c = conn_new(fd, use_tls, timeout_ms);
    if (!c) return NULL;
    plat_sock_set_timeout(fd, timeout_ms);
    return use_tls ? conn_start_tls(c, true) : c;
}

conn_t *conn_connect(const char *ip, uint16_t port, bool use_tls, int timeout_ms)
{
    ls_sock fd = plat_tcp_connect(ip, port, timeout_ms);
    if (fd < 0) return NULL;
    conn_t *c = conn_new(fd, use_tls, timeout_ms);
    if (!c) return NULL;
    plat_sock_set_timeout(fd, timeout_ms);
    return use_tls ? conn_start_tls(c, false) : c;
}

void conn_set_timeout(conn_t *c, int ms)
{
    c->timeout_ms = ms;
    plat_sock_set_timeout(c->fd, ms);
}

int conn_read(conn_t *c, void *buf, size_t cap)
{
    if (!c->tls) return plat_sock_recv(c->fd, buf, cap);
    int r = mbedtls_ssl_read(&c->st->ssl, buf, cap);
    if (r > 0) return r;
    if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return LS_IO_TIMEOUT;
    return LS_IO_ERROR;
}

int conn_write_all(conn_t *c, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t off = 0;
    uint64_t deadline = plat_time_ms() + (uint64_t)(c->timeout_ms > 0 ? c->timeout_ms : 10000) * 2;
    while (off < len) {
        int r;
        if (c->tls) r = mbedtls_ssl_write(&c->st->ssl, p + off, len - off);
        else        r = plat_sock_send(c->fd, p + off, len - off);
        if (r > 0) { off += (size_t)r; continue; }
        bool again = c->tls ? (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ)
                            : (r == LS_IO_TIMEOUT);
        if (again && plat_time_ms() < deadline) continue;
        return -1;
    }
    return 0;
}

bool conn_peer_fingerprint(conn_t *c, char out[65])
{
    if (!c->peer_fp[0]) return false;
    memcpy(out, c->peer_fp, 65);
    return true;
}

void conn_close(conn_t *c)
{
    if (!c) return;
    if (c->st) {
        mbedtls_ssl_close_notify(&c->st->ssl);
        tls_state_free(c->st);
        free(c->st);
    }
    plat_sock_close(c->fd);
    free(c);
}

/* ================================================================== SHA-256 */

struct ls_sha256 { mbedtls_sha256_context ctx; };

ls_sha256 *sha256_new(void)
{
    ls_sha256 *h = malloc(sizeof *h);
    if (!h) return NULL;
    mbedtls_sha256_init(&h->ctx);
    if (mbedtls_sha256_starts(&h->ctx, 0) != 0) { free(h); return NULL; }
    return h;
}

void sha256_update(ls_sha256 *h, const void *data, size_t len)
{
    if (h) mbedtls_sha256_update(&h->ctx, data, len);
}

void sha256_finish_hex(ls_sha256 *h, char out[65])
{
    uint8_t d[32] = { 0 };
    if (h) {
        mbedtls_sha256_finish(&h->ctx, d);
        mbedtls_sha256_free(&h->ctx);
        free(h);
    }
    ls_hex(d, sizeof d, out);
}

void sha256_discard(ls_sha256 *h)
{
    if (!h) return;
    mbedtls_sha256_free(&h->ctx);
    free(h);
}

/* ================================================================== self tests (Phase 1) */

const char *tls_version_string(void)
{
    return MBEDTLS_VERSION_STRING_FULL;
}

bool tls_selftest_sha256(void)
{
    static const uint8_t expect[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t out[32];
    int r = mbedtls_sha256((const unsigned char *)"abc", 3, out, 0);
    return r == 0 && memcmp(out, expect, sizeof out) == 0;
}

/* --- in-memory pipe so a TLS client and server can talk without any network --- */
#define PIPE_CAP 24000
typedef struct { uint8_t data[PIPE_CAP]; size_t len; } fifo;
typedef struct { fifo *rx, *tx; } pipe_end;

static int pipe_send(void *ctx, const unsigned char *buf, size_t n)
{
    pipe_end *e = ctx;
    if (e->tx->len + n > PIPE_CAP) return MBEDTLS_ERR_SSL_WANT_WRITE;
    memcpy(e->tx->data + e->tx->len, buf, n);
    e->tx->len += n;
    return (int)n;
}

static int pipe_recv(void *ctx, unsigned char *buf, size_t n)
{
    pipe_end *e = ctx;
    if (e->rx->len == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    size_t k = n < e->rx->len ? n : e->rx->len;
    memcpy(buf, e->rx->data, k);
    memmove(e->rx->data, e->rx->data + k, e->rx->len - k);
    e->rx->len -= k;
    return (int)k;
}

bool tls_selftest_loopback(double *handshake_ms)
{
    bool       ok = false;
    tls_state *cli = calloc(1, sizeof *cli);
    tls_state *srv = calloc(1, sizeof *srv);
    fifo      *c2s = calloc(1, sizeof *c2s);
    fifo      *s2c = calloc(1, sizeof *s2c);
    bool       cli_ok = false, srv_ok = false;
    if (!cli || !srv || !c2s || !s2c) goto done;

    cli_ok = tls_state_init(cli, false);
    srv_ok = tls_state_init(srv, true);
    if (!cli_ok || !srv_ok) goto done;

    pipe_end ce = { s2c, c2s }, se = { c2s, s2c };
    mbedtls_ssl_set_bio(&cli->ssl, &ce, pipe_send, pipe_recv, NULL);
    mbedtls_ssl_set_bio(&srv->ssl, &se, pipe_send, pipe_recv, NULL);

    uint64_t t0 = plat_time_ms();
    bool cdone = false, sdone = false;
    for (int i = 0; i < 1000 && !(cdone && sdone); i++) {
        int r;
        if (!cdone) {
            r = mbedtls_ssl_handshake(&cli->ssl);
            if (r == 0) cdone = true;
            else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) { log_mbed("loopback client handshake", r); goto done; }
        }
        if (!sdone) {
            r = mbedtls_ssl_handshake(&srv->ssl);
            if (r == 0) sdone = true;
            else if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) { log_mbed("loopback server handshake", r); goto done; }
        }
    }
    if (!(cdone && sdone)) { ls_log("loopback handshake did not finish"); goto done; }
    if (handshake_ms) *handshake_ms = (double)(plat_time_ms() - t0);

    /* application data both ways */
    unsigned char b[16];
    if (mbedtls_ssl_write(&cli->ssl, (const unsigned char *)"ping", 4) != 4) goto done;
    if (mbedtls_ssl_read(&srv->ssl, b, sizeof b) != 4 || memcmp(b, "ping", 4)) goto done;
    if (mbedtls_ssl_write(&srv->ssl, (const unsigned char *)"pong", 4) != 4) goto done;
    if (mbedtls_ssl_read(&cli->ssl, b, sizeof b) != 4 || memcmp(b, "pong", 4)) goto done;

    ls_log("loopback TLS: %s, %s", mbedtls_ssl_get_version(&cli->ssl), mbedtls_ssl_get_ciphersuite(&cli->ssl));
    ok = true;
done:
    if (cli_ok) tls_state_free(cli);
    if (srv_ok) tls_state_free(srv);
    free(cli); free(srv); free(c2s); free(s2c);
    return ok;
}
