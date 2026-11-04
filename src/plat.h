/*
 * plat.h - the ONLY place where the PS Vita (VitaSDK) and the PC test build differ.
 *
 *   plat_vita.c   -> sceNet / sceKernel / sceIo   (the real thing)
 *   plat_posix.c  -> BSD sockets / pthreads       (host test build, see host/)
 *
 * Everything else in src/ is plain portable C.
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_PLAT_H
#define LS_PLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int ls_sock;
#define LS_BAD_SOCK   (-1)
#define LS_IO_ERROR   (-1)   /* hard error                                        */
#define LS_IO_TIMEOUT (-2)   /* nothing to read / write right now (or timed out)   */

typedef struct {
    char     ip[16];         /* dotted quad */
    uint16_t port;
} ls_endpoint;

/* ---------- lifecycle ---------- */
void plat_init(void);                        /* Vita: raise the CPU clock to 444 MHz (crypto is CPU heavy) */
void plat_keep_awake(void);                  /* call every few seconds: stops auto-suspend / screen off */
void plat_battery(int *percent, bool *charging);   /* percent -1 = unknown */
bool plat_local_clock(int *hour, int *minute);     /* wall-clock time in the Vita's own time zone; false if unknown */
bool plat_can_launch(void);                 /* this build can open apps (the button is only offered then) */
bool plat_launch_app(const char *titleid);   /* opens an installed app (and ends this one); false if that is not possible */
void plat_chime(void);                       /* a short "finished" sound; returns at once (plays in the background) */

/* ---------- time / randomness ---------- */
uint64_t plat_time_ms(void);                 /* monotonic */
void     plat_sleep_ms(unsigned ms);
bool     plat_random(void *buf, size_t len); /* cryptographic random bytes from the OS */

/* ---------- threads ---------- */
typedef struct plat_mutex plat_mutex;
plat_mutex *plat_mutex_create(void);
void        plat_mutex_lock(plat_mutex *m);
void        plat_mutex_unlock(plat_mutex *m);
/* Starts a detached thread. Returns false if it could not be created. */
bool plat_thread_spawn(const char *name, void (*fn)(void *), void *arg, size_t stack_bytes);

/* ---------- files ---------- */
const char *plat_data_dir(void);             /* "ux0:data/VitaSend" on the Vita (an old LocalSendVita folder is renamed) */
bool        plat_mkdirs(const char *dir);

typedef struct {
    char     name[256];
    bool     is_dir;
    uint64_t size;
} plat_dirent;
/* Lists a folder (without "." and ".."). Returns the entry count (may be 0) or -1 if it cannot be opened.
 * *out is malloc'd - free() it. */
int         plat_list_dir(const char *path, plat_dirent **out);
/* Storage devices that exist ("ux0:", "uma0:", ... on the Vita; "/" on the PC). Returns the count. */
int         plat_list_roots(char out[][16], int max);
/* Folder the file browser opens first, or NULL to show the list of storage devices. */
const char *plat_browse_start(void);
bool        plat_path_info(const char *path, bool *is_dir, uint64_t *size);   /* false if it does not exist */
/* Free bytes on the storage that holds `path` (the folder may not exist yet); UINT64_MAX if unknown. */
uint64_t    plat_free_space(const char *path);
bool        plat_rename(const char *from, const char *to);
bool        plat_remove(const char *path);                                    /* a file */
bool        plat_rmdir(const char *path);                                     /* an EMPTY folder */

/* ---------- installing .vpk packages ---------- */
/* Scratch folder the VPK installer unpacks into ("ux0:data/pkg" on the Vita - the same one VitaShell uses). */
const char *plat_pkg_dir(void);
/* Title id of this very app (it must never be installed over itself while running). */
const char *plat_own_title_id(void);
/* Hands an unpacked package folder (with sce_sys/param.sfo and sce_sys/package/head.bin) to the system installer, which
 * moves it to ux0:app/<TITLEID> and adds the bubble to the LiveArea. Blocks for a few seconds. Returns 0 on success,
 * otherwise fills `why` with a readable error. Needs the app to be built "unsafe" (see README). */
int         plat_promote(const char *dir, char *why, size_t cap);

/* ---------- network ---------- */
bool plat_net_init(void);
void plat_net_shutdown(void);
bool plat_net_local_ip(char out[16]);        /* false while Wi-Fi is not connected */
const char *plat_net_error_text(void);       /* why the last socket open/bind/listen/connect failed, e.g. "bind: 0x80410130" */

/* UDP: sockets are non-blocking. recvfrom returns >0 bytes, LS_IO_TIMEOUT if nothing is waiting. */
ls_sock plat_udp_open(uint16_t port);        /* port 0 = any (send-only) */
bool    plat_udp_join_group(ls_sock s, const char *group, const char *local_ip);
int     plat_udp_recvfrom(ls_sock s, void *buf, size_t cap, ls_endpoint *from);
int     plat_udp_sendto(ls_sock s, const void *buf, size_t len, const char *ip, uint16_t port);

/* TCP listener is non-blocking; accepted / connected sockets are blocking with a timeout. */
ls_sock plat_tcp_listen(uint16_t port, int backlog);            /* port 0 = any free port */
uint16_t plat_sock_local_port(ls_sock s);                        /* the port a socket is bound to, 0 if unknown */
ls_sock plat_tcp_accept(ls_sock listener, ls_endpoint *peer);   /* LS_IO_TIMEOUT if none pending */
ls_sock plat_tcp_connect(const char *ip, uint16_t port, int timeout_ms);
void    plat_sock_set_timeout(ls_sock s, int ms);
int     plat_sock_recv(ls_sock s, void *buf, size_t cap);       /* >0, 0 = peer closed, LS_IO_* */
int     plat_sock_send(ls_sock s, const void *buf, size_t len); /* >0 bytes sent, LS_IO_*       */
void    plat_sock_close(ls_sock s);

#ifdef LS_HOST
void plat_posix_set_data_dir(const char *dir);
void plat_posix_set_browse_start(const char *dir);
void plat_posix_set_free_space(uint64_t bytes);          /* tests: pretend the card has only this much room */
#endif

#endif
