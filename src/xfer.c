/*
 * xfer.c - The one running transfer and its progress: written by the network threads, read as snapshots by the UI.
 *
 * Author: trashman (VitaSend)
 */
#include "xfer.h"
#include "plat.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static xfer_status    s_x;
static bool           s_busy;
static plat_mutex    *s_lock;
static volatile bool  s_cancel;
static uint64_t       s_win_t0, s_win_bytes;

void xfer_init(void)
{
    if (!s_lock) s_lock = plat_mutex_create();
}

static void lock(void) { plat_mutex_lock(s_lock); }
static void unlock(void) { plat_mutex_unlock(s_lock); }

bool xfer_begin(bool outgoing, const char *peer)
{
    lock();
    if (s_busy) { unlock(); return false; }
    s_busy = true;
    uint32_t serial = s_x.serial + 1;
    memset(&s_x, 0, sizeof s_x);
    s_x.serial = serial;
    s_x.outgoing = outgoing;
    s_x.state = XF_WAITING;
    ls_strlcpy(s_x.peer, peer, sizeof s_x.peer);
    s_cancel = false;
    s_win_t0 = plat_time_ms();
    s_win_bytes = 0;
    unlock();
    return true;
}

bool xfer_busy(void)
{
    lock();
    bool b = s_busy;
    unlock();
    return b;
}

void xfer_snapshot(xfer_status *out)
{
    lock();
    *out = s_x;
    unlock();
}

void xfer_set_message(const char *fmt, ...)
{
    char tmp[sizeof s_x.message];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    lock();
    ls_strlcpy(s_x.message, tmp, sizeof s_x.message);
    unlock();
}

void xfer_set_state(xf_state st)
{
    lock();
    s_x.state = st;
    unlock();
}

void xfer_set_totals(int files, uint64_t bytes)
{
    lock();
    s_x.files_total = files;
    s_x.bytes_total = bytes;
    unlock();
}

void xfer_set_current(const char *name)
{
    lock();
    ls_strlcpy(s_x.current, name, sizeof s_x.current);
    unlock();
}

void xfer_add_bytes(uint64_t n)
{
    lock();
    s_x.bytes_done += n;
    s_win_bytes += n;
    uint64_t now = plat_time_ms();
    if (now - s_win_t0 >= 700) {
        uint64_t inst = s_win_bytes * 1000u / (now - s_win_t0);
        s_x.speed_bps = s_x.speed_bps ? (uint32_t)((s_x.speed_bps + inst) / 2) : (uint32_t)inst;
        s_win_t0 = now;
        s_win_bytes = 0;
    }
    unlock();
}

void xfer_file_done(void)
{
    lock();
    s_x.files_done++;
    unlock();
}

void xfer_end(xf_state final_state, const char *fmt, ...)
{
    char tmp[sizeof s_x.message];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    lock();
    s_x.state = final_state;
    s_x.speed_bps = 0;
    ls_strlcpy(s_x.message, tmp, sizeof s_x.message);
    s_busy = false;
    unlock();
}

void xfer_end_silent(void)
{
    lock();
    s_x.state = XF_IDLE;
    s_busy = false;
    unlock();
}

void xfer_request_cancel(void) { s_cancel = true; }
volatile bool *xfer_cancel_flag(void) { return &s_cancel; }
