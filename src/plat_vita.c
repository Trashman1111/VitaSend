/*
 * plat_vita.c - PS Vita implementation of plat.h (VitaSDK).
 *
 * Uses the native sceNet / sceKernel APIs directly instead of newlib's BSD-socket shim, so the
 * behaviour (non-blocking flags, timeouts, multicast) is exactly what the system software documents.
 *
 * Author: trashman (VitaSend)
 */
#include "plat.h"
#include "util.h"

#include <math.h>
#ifdef LS_HAVE_APPMGR
#include <psp2/appmgr.h>
#endif
#include <psp2/audioout.h>
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/promoterutil.h>
#ifdef LS_HAVE_RTC
#include <psp2/rtc.h>
#endif
#include <psp2/sysmodule.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef LS_TITLE_ID
#define LS_TITLE_ID "LSND00001"      /* normally passed in by CMakeLists.txt */
#endif

#define NET_MEMORY_BYTES   (1024 * 1024)
#define DEFAULT_PRIORITY   0x10000100          /* SCE_KERNEL_DEFAULT_PRIORITY_USER */

/* The memory handed to sceNetInit: a static, 64-byte aligned block (some firmware dislikes malloc'ed memory). */
static char s_net_pool[NET_MEMORY_BYTES] __attribute__((aligned(64)));
static bool s_net_up;

/* ------------------------------------------------------------------ lifecycle / time */

void plat_init(void)
{
    scePowerSetArmClockFrequency(444);
}

void plat_keep_awake(void)
{
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
}

void plat_battery(int *percent, bool *charging)
{
    int p = scePowerGetBatteryLifePercent();
    *percent = (p >= 0 && p <= 100) ? p : -1;
    *charging = scePowerIsBatteryCharging() ? true : false;
}

/* The chime: two soft bell-like notes (D6, then A6) made of sine waves with a fast attack and a long decay. It is
 * generated on the fly (no sound file needed) and played on the "main" audio port from a short-lived thread. */
static volatile int s_chime_busy;

static void chime_thread(void *arg)
{
    (void)arg;
    enum { GRAIN = 1024, RATE = 48000 };
    int port = sceAudioOutOpenPort((SceAudioOutPortType)0 /* MAIN */, GRAIN, RATE, (SceAudioOutMode)0 /* MONO */);
    if (port >= 0) {
        static const struct { float hz; float start; } notes[2] = { { 1174.7f, 0.0f }, { 1760.0f, 0.16f } };
        const float total = 0.95f;
        int frames = (int)(total * RATE);
        static int16_t buf[GRAIN];
        for (int pos = 0; pos < frames; pos += GRAIN) {
            for (int i = 0; i < GRAIN; i++) {
                float t = (float)(pos + i) / RATE, v = 0;
                for (int n = 0; n < 2; n++) {
                    float tt = t - notes[n].start;
                    if (tt < 0) continue;
                    float env = (tt < 0.004f ? tt / 0.004f : 1.0f) * expf(-tt * 5.5f);
                    v += env * (sinf(6.2831853f * notes[n].hz * tt) + 0.25f * sinf(6.2831853f * notes[n].hz * 2.01f * tt));
                }
                buf[i] = (int16_t)(v * 7000.0f);
            }
            sceAudioOutOutput(port, buf);
        }
        sceAudioOutOutput(port, NULL);                 /* wait until everything has been played */
        sceAudioOutReleasePort(port);
    }
    s_chime_busy = 0;
}

bool plat_local_clock(int *hour, int *minute)
{
#ifdef LS_HAVE_RTC
    SceDateTime dt;
    if (sceRtcGetCurrentClockLocalTime(&dt) == 0 && dt.year >= 2000) { *hour = dt.hour; *minute = dt.minute; return true; }
    return false;
#else
    time_t t = time(NULL);                                        /* UTC (this SDK has no RTC stub library) */
    struct tm tmv;
    if (t < 1000000000 || !gmtime_r(&t, &tmv)) return false;
    *hour = tmv.tm_hour;
    *minute = tmv.tm_min;
    return true;
#endif
}

bool plat_can_launch(void)
{
#ifdef LS_HAVE_APPMGR
    return true;
#else
    return false;
#endif
}

/* The VitaShell way: ask the system to start the title (twice, the first request is sometimes ignored), then leave. */
bool plat_launch_app(const char *titleid)
{
#ifdef LS_HAVE_APPMGR
    char uri[48];
    if (!titleid || !titleid[0]) return false;
    snprintf(uri, sizeof uri, "psgm:play?titleid=%s", titleid);
    sceKernelDelayThread(10000);
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelDelayThread(10000);
    sceAppMgrLaunchAppByUri(0xFFFFF, uri);
    sceKernelExitProcess(0);
    return true;
#else
    (void)titleid;
    return false;
#endif
}

void plat_chime(void)
{
    if (s_chime_busy) return;
    s_chime_busy = 1;
    if (!plat_thread_spawn("chime", chime_thread, NULL, 64 * 1024)) s_chime_busy = 0;
}

uint64_t plat_time_ms(void)
{
    return sceKernelGetProcessTimeWide() / 1000u;
}

void plat_sleep_ms(unsigned ms)
{
    sceKernelDelayThread(ms * 1000u);
}

bool plat_random(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        size_t n = len > 64 ? 64 : len;
        if (sceKernelGetRandomNumber(p, n) < 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

/* ------------------------------------------------------------------ threads */

struct plat_mutex { SceUID id; };

plat_mutex *plat_mutex_create(void)
{
    plat_mutex *m = malloc(sizeof *m);
    if (!m) return NULL;
    m->id = sceKernelCreateMutex("ls_mutex", 0, 0, NULL);
    if (m->id < 0) { free(m); return NULL; }
    return m;
}

void plat_mutex_lock(plat_mutex *m)   { sceKernelLockMutex(m->id, 1, NULL); }
void plat_mutex_unlock(plat_mutex *m) { sceKernelUnlockMutex(m->id, 1); }

typedef struct { void (*fn)(void *); void *arg; } vtramp;

static int thread_entry(SceSize args, void *argp)
{
    (void)args;
    vtramp *t = *(vtramp **)argp;            /* sceKernelStartThread copied our pointer into argp */
    void (*fn)(void *) = t->fn;
    void *arg = t->arg;
    free(t);
    fn(arg);
    sceKernelExitDeleteThread(0);
    return 0;
}

bool plat_thread_spawn(const char *name, void (*fn)(void *), void *arg, size_t stack_bytes)
{
    vtramp *t = malloc(sizeof *t);
    if (!t) return false;
    t->fn = fn;
    t->arg = arg;
    SceUID th = sceKernelCreateThread(name, thread_entry, DEFAULT_PRIORITY, (SceSize)stack_bytes, 0, 0, NULL);
    if (th < 0) { free(t); return false; }
    if (sceKernelStartThread(th, sizeof t, &t) < 0) {
        sceKernelDeleteThread(th);
        free(t);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ files */

#define DATA_DIR     "ux0:data/VitaSend"
#define OLD_DATA_DIR "ux0:data/LocalSendVita"      /* the folder up to v0.7.0x3 */

static bool dir_exists(const char *path)
{
    SceUID d = sceIoDopen(path);
    if (d < 0) return false;
    sceIoDclose(d);
    return true;
}

/* The app folder is ux0:data/VitaSend. An existing LocalSendVita folder (settings, certificate, history, received
 * files) is renamed once, so nothing is lost; if that fails the old folder simply keeps being used. */
const char *plat_data_dir(void)
{
    static const char *chosen;
    if (!chosen) {
        chosen = DATA_DIR;
        if (!dir_exists(DATA_DIR) && dir_exists(OLD_DATA_DIR) && sceIoRename(OLD_DATA_DIR, DATA_DIR) < 0) chosen = OLD_DATA_DIR;
    }
    return chosen;
}

bool plat_mkdirs(const char *dir)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", dir);
    char *colon = strchr(tmp, ':');
    for (char *p = (colon ? colon + 1 : tmp); *p; p++) {
        if (*p == '/' && p != colon + 1) {
            *p = 0;
            sceIoMkdir(tmp, 0777);
            *p = '/';
        }
    }
    sceIoMkdir(tmp, 0777);
    SceUID d = sceIoDopen(tmp);
    if (d < 0) return false;
    sceIoDclose(d);
    return true;
}

static const char *const k_roots[] = { "ux0:", "uma0:", "imc0:", "xmc0:", "ur0:", "grw0:" };

int plat_list_roots(char out[][16], int max)
{
    int n = 0;
    for (size_t i = 0; i < sizeof k_roots / sizeof k_roots[0] && n < max; i++) {
        SceUID d = sceIoDopen(k_roots[i]);
        if (d < 0) continue;                                 /* that device is not present / not accessible */
        sceIoDclose(d);
        snprintf(out[n++], 16, "%s", k_roots[i]);
    }
    return n;
}

const char *plat_browse_start(void)
{
    return NULL;                                             /* start at the list of storage devices */
}

int plat_list_dir(const char *path, plat_dirent **out)
{
    SceUID d = sceIoDopen(path);
    if (d < 0) return -1;
    size_t cap = 64, n = 0;
    plat_dirent *v = malloc(cap * sizeof *v);
    if (!v) { sceIoDclose(d); return -1; }
    SceIoDirent e;
    for (;;) {
        memset(&e, 0, sizeof e);
        int r = sceIoDread(d, &e);
        if (r <= 0) break;
        if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, "..")) continue;
        if (n == cap) {
            cap *= 2;
            plat_dirent *nv = realloc(v, cap * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        plat_dirent *o = &v[n++];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", e.d_name);
        o->is_dir = SCE_S_ISDIR(e.d_stat.st_mode);
        o->size = o->is_dir ? 0 : (uint64_t)e.d_stat.st_size;
    }
    sceIoDclose(d);
    *out = v;
    return (int)n;
}

bool plat_path_info(const char *path, bool *is_dir, uint64_t *size)
{
    SceIoStat st;
    memset(&st, 0, sizeof st);
    if (sceIoGetstat(path, &st) < 0) return false;
    bool dir = SCE_S_ISDIR(st.st_mode);
    if (is_dir) *is_dir = dir;
    if (size) *size = dir ? 0 : (uint64_t)st.st_size;
    return true;
}

uint64_t plat_free_space(const char *path)
{
    char dev[16];
    const char *colon = strchr(path, ':');
    if (!colon || colon - path >= (long)sizeof dev - 2) return UINT64_MAX;
    memcpy(dev, path, (size_t)(colon - path) + 1);
    dev[colon - path + 1] = 0;                               /* "ux0:" */
    SceIoDevInfo info;
    memset(&info, 0, sizeof info);
    if (sceIoDevctl(dev, 0x3001, NULL, 0, &info, sizeof info) < 0) return UINT64_MAX;
    return info.free_size > 0 ? (uint64_t)info.free_size : UINT64_MAX;
}

bool plat_rename(const char *from, const char *to) { return sceIoRename(from, to) >= 0; }
bool plat_remove(const char *path)                 { return sceIoRemove(path) >= 0; }
bool plat_rmdir(const char *path)                  { return sceIoRmdir(path) >= 0; }

/* ------------------------------------------------------------------ installing .vpk packages
 *
 * Same procedure as VitaShell: load the system's PAF + ScePromoterUtil modules, then ask the promoter to turn the unpacked
 * folder (ux0:data/pkg) into an installed app. These internal modules can only be loaded by an "unsafe" (extended
 * permission) homebrew - see CMakeLists.txt, option LS_UNSAFE. */

const char *plat_pkg_dir(void) { return "ux0:data/pkg"; }
const char *plat_own_title_id(void) { return LS_TITLE_ID; }

int plat_promote(const char *dir, char *why, size_t cap)
{
#ifdef LS_NO_PROMOTER
    (void)dir;
    snprintf(why, cap, "This build has no installer support (VitaSDK lacks ScePromoterUtil)");
    return -1;
#else
    static uint32_t argp[] = { 0x180000, (uint32_t)-1, (uint32_t)-1, 1, (uint32_t)-1, (uint32_t)-1 };
    int result = -1;
    int res;
    bool paf = false, prom = false, inited = false;
    SceSysmoduleOpt opt;
    memset(&opt, 0, sizeof opt);
    opt.flags = sizeof opt;
    opt.result = &result;
    opt.unused[0] = -1;
    opt.unused[1] = -1;

    res = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof argp, argp, &opt);
    if (res < 0) { snprintf(why, cap, "Cannot load the system module PAF (0x%08X) - is the app installed as unsafe?", (unsigned)res); goto out; }
    paf = true;

    res = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (res < 0) { snprintf(why, cap, "Cannot load ScePromoterUtil (0x%08X)", (unsigned)res); goto out; }
    prom = true;

    res = scePromoterUtilityInit();
    if (res < 0) { snprintf(why, cap, "Installer init failed (0x%08X)", (unsigned)res); goto out; }
    inited = true;

    res = scePromoterUtilityPromotePkgWithRif(dir, 1);
    if (res < 0) { snprintf(why, cap, "The system refused the package (0x%08X)", (unsigned)res); goto out; }
    res = 0;

out:
    if (inited) scePromoterUtilityExit();
    if (prom) sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (paf) {
        SceSysmoduleOpt uo;                 /* like VitaShell: all zero */
        memset(&uo, 0, sizeof uo);
        sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, &uo);
    }
    return res < 0 ? res : 0;
#endif
}

/* ------------------------------------------------------------------ network */

static bool would_block(int r)
{
    return r == (int)SCE_NET_ERROR_EAGAIN || r == (int)SCE_NET_ERROR_EWOULDBLOCK || r == (int)SCE_NET_ERROR_ETIMEDOUT;
}

static void set_int_opt(int s, int level, int opt, int value)
{
    sceNetSetsockopt(s, level, opt, &value, sizeof value);
}

static void fill_addr(SceNetSockaddrIn *a, const char *ip, uint16_t port)
{
    memset(a, 0, sizeof *a);
    a->sin_len = sizeof *a;
    a->sin_family = SCE_NET_AF_INET;
    a->sin_port = sceNetHtons(port);
    if (ip) sceNetInetPton(SCE_NET_AF_INET, ip, &a->sin_addr);
}

/* ---- socket helpers: firmware versions differ, so every step is tried more than one way, and the exact error
 *      code of the failure is kept so the app can show it on screen (see plat_net_error_text) ---- */

static char s_net_err[128];

static const char *err_name(int code)
{
    switch ((unsigned)code) {
    case 0x80410130u: return "(in use)";
    case 0x80410131u: return "(addr n/a)";
    case 0x8041010Du: return "(denied)";
    case 0x80410116u: return "(invalid)";
    case 0x8041012Fu: return "(af n/a)";
    case 0x80410137u: return "(no memory)";
    case 0x8041013Cu: return "(refused)";
    case 0x8041013Du: return "(unreachable)";
    default:          return "";
    }
}

static void note_err(const char *stage, int code)
{
    snprintf(s_net_err, sizeof s_net_err, "%s %08X%s", stage, (unsigned)code, err_name(code));
}

const char *plat_net_error_text(void)
{
    return s_net_err[0] ? s_net_err : "no error recorded";
}

/* The protocol argument: some firmware wants the explicit one, some want 0 - try both. */
static int open_sock(const char *name, int type)
{
    int proto = type == SCE_NET_SOCK_STREAM ? SCE_NET_IPPROTO_TCP : SCE_NET_IPPROTO_UDP;
    int s = sceNetSocket(name, SCE_NET_AF_INET, type, proto);
    if (s >= 0) return s;
    int first = s;
    s = sceNetSocket(name, SCE_NET_AF_INET, type, 0);
    if (s >= 0) return s;
    note_err("socket", first);
    return s;
}

/* Binds (and for TCP listens on) 0.0.0.0:port. Tries a few variations of options and address layout. If all fail, the
 * error text lists every code (and whether a bind to "any free port" works) so the screen shows what is going on. */
static ls_sock bind_sock(const char *name, int type, uint16_t port, int backlog)
{
    static const struct { bool reuse_addr, reuse_port, zero_len, own_ip; } variant[] = {
        { true,  false, false, false },
        { false, false, false, false },
        { true,  false, true,  false },
        { true,  true,  false, false },
        { true,  false, false, true  },          /* bind to the Wi-Fi address instead of 0.0.0.0 */
    };
    int codes[5] = { 0, 0, 0, 0, 0 };
    char own_ip[16] = "";
    plat_net_local_ip(own_ip);
    const char *stage = "bind";
    for (size_t i = 0; i < sizeof variant / sizeof variant[0]; i++) {
        int s = open_sock(name, type);
        if (s < 0) return LS_BAD_SOCK;                           /* open_sock recorded the error */
        if (variant[i].reuse_addr) set_int_opt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, 1);
        if (variant[i].reuse_port) set_int_opt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEPORT, 1);
        SceNetSockaddrIn a;
        fill_addr(&a, NULL, port);
        a.sin_addr.s_addr = 0;                                   /* INADDR_ANY */
        if (variant[i].own_ip) {
            if (!own_ip[0]) { sceNetSocketClose(s); continue; }
            sceNetInetPton(SCE_NET_AF_INET, own_ip, &a.sin_addr);
        }
        if (variant[i].zero_len) a.sin_len = 0;
        stage = "bind";
        int r = sceNetBind(s, (SceNetSockaddr *)&a, sizeof a);
        if (r >= 0 && type == SCE_NET_SOCK_STREAM) {
            stage = "listen";
            r = sceNetListen(s, backlog);
        }
        if (r >= 0) {
            set_int_opt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, 1);
            return s;
        }
        codes[i] = r;
        sceNetSocketClose(s);
    }

    /* does binding to "any free port" work at all? (then only this one port is refused) */
    const char *p0 = "n/a";
    if (port != 0) {
        int s = open_sock(name, type);
        if (s >= 0) {
            SceNetSockaddrIn a;
            fill_addr(&a, NULL, 0);
            a.sin_addr.s_addr = 0;
            p0 = sceNetBind(s, (SceNetSockaddr *)&a, sizeof a) >= 0 ? "ok" : "fails";
            sceNetSocketClose(s);
        }
    }
    snprintf(s_net_err, sizeof s_net_err, "%s %08X%s %08X %08X %08X %08X p0=%s", stage, (unsigned)codes[0],
             err_name(codes[0]), (unsigned)codes[1], (unsigned)codes[2], (unsigned)codes[3], (unsigned)codes[4], p0);
    return LS_BAD_SOCK;
}

uint16_t plat_sock_local_port(ls_sock s)
{
    SceNetSockaddrIn a;
    unsigned int l = sizeof a;
    memset(&a, 0, sizeof a);
    if (sceNetGetsockname(s, (SceNetSockaddr *)&a, &l) < 0) return 0;
    return sceNetNtohs(a.sin_port);
}

bool plat_net_init(void)
{
    if (s_net_up) return true;
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    SceNetInitParam p;
    memset(&p, 0, sizeof p);
    p.memory = s_net_pool;
    p.size = NET_MEMORY_BYTES;
    p.flags = 0;
    int r = sceNetInit(&p);
    if (r < 0) {
        note_err("sceNetInit", r);
        return false;
    }
    sceNetCtlInit();
    s_net_up = true;
    return true;
}

void plat_net_shutdown(void)
{
    if (!s_net_up) return;
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    s_net_up = false;
}

bool plat_net_local_ip(char out[16])
{
    SceNetCtlInfo info;
    memset(&info, 0, sizeof info);
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) < 0) return false;
    snprintf(out, 16, "%s", info.ip_address);
    return out[0] && strcmp(out, "0.0.0.0") != 0;
}

/* Switching Wi-Fi on from inside an app.
 *
 * Sony gives apps no call for this (the only official switch is Settings > Flight Mode). The system module SceWlanBt exports
 * sceWlanSetConfiguration() to user code; vitacompanion uses it as "request 0 = Wi-Fi off, request 1 = Wi-Fi on". That is
 * reverse-engineered, not documented, so this is written defensively: the call is linked weakly (the build simply leaves it out
 * if this VitaSDK has no stub library for it), its answer is logged word for word, and the caller (wifi.c) falls back to
 * "turn it on in the Vita settings" if no network address appears. The system's own check dialog (SceNetCheckDialog) is not
 * used: it needs the GPU-based common-dialog rendering that this software-drawn app does not have. */
#ifdef LS_HAVE_WLAN
extern int sceWlanSetConfiguration(int request) __attribute__((weak));
#endif

int plat_wifi_turn_on(char *detail, size_t cap)
{
#ifdef LS_HAVE_WLAN
    if (!sceWlanSetConfiguration) { snprintf(detail, cap, "this firmware has no such call"); return -1; }
    int r = sceWlanSetConfiguration(1);
    ls_log("Wi-Fi: sceWlanSetConfiguration(1) returned 0x%08X", (unsigned)r);
    if (r < 0) { snprintf(detail, cap, "the system said 0x%08X", (unsigned)r); return -1; }
    snprintf(detail, cap, "the system said 0x%08X", (unsigned)r);
    return 0;
#else
    snprintf(detail, cap, "this build has no switch-on support");
    return -1;
#endif
}

/* The Settings app can be started with its own URI scheme ("settings_dlg:..."). The community list (vita-uriCaller) only has the
 * sub-pages "wifi::wps" and "wifi::manual" (the second one opens "Configure Access Points", which is one step too far); the page
 * with the Wi-Fi switch itself is not on it, so "settings_dlg:wifi" is a guess. config.json can override it ("wifiSettingsUri") so
 * other addresses can be tried without a new build. The answer is logged, and a plain "settings_dlg:" (the Settings main list) is
 * tried when the address is refused. Like starting any other app from here, a success ends this app. */
bool plat_open_wifi_settings(const char *uri)
{
#ifdef LS_HAVE_APPMGR
    const char *uris[2] = { (uri && uri[0]) ? uri : "settings_dlg:wifi", "settings_dlg:" };
    for (int i = 0; i < 2; i++) {
        sceKernelDelayThread(10000);
        int r = sceAppMgrLaunchAppByUri(0xFFFFF, uris[i]);
        ls_log("Wi-Fi: launching %s returned 0x%08X", uris[i], (unsigned)r);
        if (r >= 0) {
            sceKernelDelayThread(300000);
            sceKernelExitProcess(0);
            return true;
        }
    }
    return false;
#else
    (void)uri;
    return false;
#endif
}

ls_sock plat_udp_open(uint16_t port)
{
    ls_sock b = bind_sock("ls_udp", SCE_NET_SOCK_DGRAM, port, 0);
    if (b >= 0 || port) return b;
    int s = open_sock("ls_udp", SCE_NET_SOCK_DGRAM);               /* port 0 (send-only): an unbound socket still works for sendto() */
    if (s < 0) return LS_BAD_SOCK;
    set_int_opt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, 1);
    return s;
}

bool plat_udp_join_group(ls_sock s, const char *group, const char *local_ip)
{
    SceNetIpMreq m;
    memset(&m, 0, sizeof m);
    sceNetInetPton(SCE_NET_AF_INET, group, &m.imr_multiaddr);
    if (local_ip) sceNetInetPton(SCE_NET_AF_INET, local_ip, &m.imr_interface);
    return sceNetSetsockopt(s, SCE_NET_IPPROTO_IP, SCE_NET_IP_ADD_MEMBERSHIP, &m, sizeof m) >= 0;
}

int plat_udp_recvfrom(ls_sock s, void *buf, size_t cap, ls_endpoint *from)
{
    SceNetSockaddrIn a;
    unsigned int l = sizeof a;
    memset(&a, 0, sizeof a);
    int r = sceNetRecvfrom(s, buf, (unsigned int)cap, 0, (SceNetSockaddr *)&a, &l);
    if (r < 0) return would_block(r) ? LS_IO_TIMEOUT : LS_IO_ERROR;
    if (from) {
        sceNetInetNtop(SCE_NET_AF_INET, &a.sin_addr, from->ip, sizeof from->ip);
        from->port = sceNetNtohs(a.sin_port);
    }
    return r;
}

int plat_udp_sendto(ls_sock s, const void *buf, size_t len, const char *ip, uint16_t port)
{
    SceNetSockaddrIn a;
    fill_addr(&a, ip, port);
    int r = sceNetSendto(s, buf, (unsigned int)len, 0, (SceNetSockaddr *)&a, sizeof a);
    return r < 0 ? LS_IO_ERROR : r;
}

ls_sock plat_tcp_listen(uint16_t port, int backlog)
{
    return bind_sock("ls_listen", SCE_NET_SOCK_STREAM, port, backlog);
}

void plat_sock_set_timeout(ls_sock s, int ms)
{
    int us = ms * 1000;                                      /* sceNet timeouts are in microseconds */
    set_int_opt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, us);
    set_int_opt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, us);
}

ls_sock plat_tcp_accept(ls_sock listener, ls_endpoint *peer)
{
    SceNetSockaddrIn a;
    unsigned int l = sizeof a;
    memset(&a, 0, sizeof a);
    int c = sceNetAccept(listener, (SceNetSockaddr *)&a, &l);
    if (c < 0) return would_block(c) ? LS_IO_TIMEOUT : LS_IO_ERROR;
    set_int_opt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, 0);  /* accepted sockets are blocking (with timeouts) */
    if (peer) {
        sceNetInetNtop(SCE_NET_AF_INET, &a.sin_addr, peer->ip, sizeof peer->ip);
        peer->port = sceNetNtohs(a.sin_port);
    }
    return c;
}

ls_sock plat_tcp_connect(const char *ip, uint16_t port, int timeout_ms)
{
    int s = open_sock("ls_conn", SCE_NET_SOCK_STREAM);
    if (s < 0) return LS_BAD_SOCK;
    plat_sock_set_timeout(s, timeout_ms);                    /* best effort: bounds connect() on most firmwares */
    SceNetSockaddrIn a;
    fill_addr(&a, ip, port);
    int r = sceNetConnect(s, (SceNetSockaddr *)&a, sizeof a);
    if (r < 0) {
        note_err("connect", r);
        sceNetSocketClose(s);
        return LS_BAD_SOCK;
    }
    return s;
}

int plat_sock_recv(ls_sock s, void *buf, size_t cap)
{
    int r = sceNetRecv(s, buf, (unsigned int)cap, 0);
    if (r >= 0) return r;
    return would_block(r) ? LS_IO_TIMEOUT : LS_IO_ERROR;
}

int plat_sock_send(ls_sock s, const void *buf, size_t len)
{
    int r = sceNetSend(s, buf, (unsigned int)len, 0);
    if (r >= 0) return r;
    return would_block(r) ? LS_IO_TIMEOUT : LS_IO_ERROR;
}

void plat_sock_close(ls_sock s)
{
    if (s >= 0) sceNetSocketClose(s);
}
