/*
 * app_install.c - installing .vpk packages: one package, or several in a queue
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"

/* ============================================================== installing a .vpk */

static char      s_inst_path[PATH_CAP];
static vpk_info  s_inst_info;
static bool      s_inst_asking;          /* true: showing what is about to be installed; false: showing the progress */
static screen_id s_inst_return;
static bool      s_inst_auto;            /* started by the "install received packages automatically" setting */
static void      iq_draw(const xfer_status *x);
static void      iq_input(uint32_t p);

void install_open(const char *path, screen_id ret)
{
    if (xfer_busy()) { toast("Wait until the transfer is finished"); return; }
    s_inst_return = ret;
    s_inst_auto = false;
    if (vpk_busy()) { s_inst_asking = false; go(SC_INSTALL); return; }
    ls_strlcpy(s_inst_path, path, sizeof s_inst_path);
    vpk_inspect(path, &s_inst_info);
    s_inst_asking = true;
    go(SC_INSTALL);
}

void install_draw(const xfer_status *x)
{
    if (iq_active()) { iq_draw(x); return; }
    vpk_status vs;
    vpk_snapshot(&vs);
    int dx, dy;
    ui_dialog_begin(SC_INSTALL | (A.tab << 8), 700, 380, under_tab, (void *)x, &dx, &dy);
    int lx = dx + 32, w = 700 - 64;

    if (s_inst_asking) {
        const vpk_info *in = &s_inst_info;
        gfx_circle((float)(lx + 28), (float)(dy + 52), 28, in->ok ? C_ACCENT_BG : GRGBA(239, 83, 80, 40));
        gfx_icon(in->ok ? IC_PACKAGE : IC_CLOSE, (float)(lx + 28), (float)(dy + 52), 30, in->ok ? C_ACCENT_HI : C_BAD);
        gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, in->ok ? "Install this app?" : "Cannot install this package");
        gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, base_name(s_inst_path));
        if (in->ok) {
            gfx_text_fit(lx, dy + 124, w, 22, GW_BOLD, C_TEXT, GA_LEFT, in->title);
            char sz[24], line[300];
            fmt_size(in->unpacked, sz, sizeof sz);
            snprintf(line, sizeof line, "%s  \xC2\xB7  Version %s  \xC2\xB7  %s  \xC2\xB7  %d files", in->titleid, in->version[0] ? in->version : "?", sz, in->files);
            gfx_text_fit(lx, dy + 156, w, 15, GW_REG, C_DIM, GA_LEFT, line);
            if (in->unsafe) {
                gfx_rrect(lx, dy + 186, w, 62, 12, GRGBA(255, 193, 77, 36));
                gfx_text_fit(lx + 16, dy + 206, w - 32, 15, GW_MED, C_WARN, GA_LEFT, "This app asks for extended permissions.");
                gfx_text_fit(lx + 16, dy + 230, w - 32, 14, GW_REG, C_TEXT, GA_LEFT, "Only install apps from sources you trust.");
            } else {
                gfx_text_fit(lx, dy + 206, w, 15, GW_REG, C_DIM, GA_LEFT, "It is unpacked on the memory card first, then added to the LiveArea.");
            }
            int w1 = ui_action_width(PB_CROSS, "Install");
            ui_action(lx, dy + 330, PB_CROSS, "Install");
            ui_action(lx + w1 + 10, dy + 330, PB_CIRCLE, "Cancel");
            ui_hints("X Install|O Cancel");
        } else {
            gfx_text_fit(lx, dy + 128, w, 17, GW_MED, C_BAD, GA_LEFT, in->err);
            ui_action(lx, dy + 330, PB_CROSS, "Close");
            ui_hints("X Close");
        }
        return;
    }

    bool ended = vs.state == VPK_DONE || vs.state == VPK_FAILED || vs.state == VPK_CANCELLED;
    gcol mc = vs.state == VPK_DONE ? C_GOOD : (vs.state == VPK_FAILED ? C_BAD : (vs.state == VPK_CANCELLED ? C_WARN : C_TEXT));
    gcol tint = vs.state == VPK_DONE ? GRGBA(102, 187, 106, 40) : (vs.state == VPK_FAILED ? GRGBA(239, 83, 80, 40) : (vs.state == VPK_CANCELLED ? GRGBA(255, 193, 77, 40) : C_ACCENT_BG));
    gfx_circle((float)(lx + 28), (float)(dy + 52), 28, tint);
    gfx_icon(vs.state == VPK_DONE ? IC_CHECK : (ended ? IC_CLOSE : IC_PACKAGE), (float)(lx + 28), (float)(dy + 52), 30, ended ? mc : C_ACCENT_HI);
    gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT,
             vs.state == VPK_DONE ? "Installed" : (vs.state == VPK_FAILED ? "Installation failed" : (vs.state == VPK_CANCELLED ? "Cancelled" : "Installing")));
    gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, vs.title[0] ? vs.title : base_name(s_inst_path));
    if (s_inst_auto) gfx_text_fit(lx, dy + 262, w, 13, GW_REG, C_FAINT, GA_LEFT, "Started automatically - Settings > Install received .vpk files");

    if (ended) {
        gfx_text_fit(lx, dy + 128, w, 17, GW_MED, mc, GA_LEFT, vs.message);
    } else {
        gfx_text_fit(lx, dy + 128, w, 17, GW_MED, C_TEXT, GA_LEFT, vs.stage);
    }
    ui_progress(lx, dy + 184, w - 70, 12, (uint64_t)vs.pct, 100, vs.state == VPK_FAILED ? C_BAD : C_ACCENT);
    char pcs[16];
    snprintf(pcs, sizeof pcs, "%d %%", vs.pct);
    gfx_text(lx + w, dy + 190, 17, GW_MED, C_TEXT, GA_RIGHT, pcs);

    if (ended) {
        int w1 = ui_action_width(PB_CROSS, "Close");
        ui_action(lx, dy + 330, PB_CROSS, "Close");
        if (vs.state == VPK_DONE && vs.titleid[0] && plat_can_launch()) {
            char lab[80];
            snprintf(lab, sizeof lab, "Open %.40s", vs.title[0] ? vs.title : vs.titleid);
            ui_action(lx + w1 + 10, dy + 330, PB_SQUARE, lab);
            ui_hints("X Close|SQ Open the app");
        } else {
            ui_hints("X Close");
        }
    } else if (vs.pct < 92) {
        ui_action(lx, dy + 330, PB_CIRCLE, "Cancel");
        ui_hints("O Cancel");
    } else {
        gfx_text(lx, dy + 346, 15, GW_REG, C_DIM, GA_LEFT, "Almost done - please keep the Vita on.");
        ui_hints("");
    }
}

void install_input(uint32_t p)
{
    if (iq_active()) { iq_input(p); return; }
    if (s_inst_asking) {
        if (p & UI_BTN_CIRCLE) go(s_inst_return);
        if (p & UI_BTN_CROSS) {
            if (!s_inst_info.ok) go(s_inst_return);
            else if (vpk_start(s_inst_path)) s_inst_asking = false;
            else toast("Cannot start the installer");
        }
        return;
    }
    vpk_status vs;
    vpk_snapshot(&vs);
    if (vs.state == VPK_RUNNING) {
        if ((p & UI_BTN_CIRCLE) && vs.pct < 92) vpk_cancel();
        return;
    }
    if ((p & UI_BTN_SQUARE) && vs.state == VPK_DONE && vs.titleid[0] && plat_can_launch()) {
        if (!plat_launch_app(vs.titleid)) toast("Cannot open the app from here - use its LiveArea bubble");
        return;
    }
    if (p & (UI_BTN_CROSS | UI_BTN_CIRCLE)) go(s_inst_return);
}


/* ---- several packages in one go: one question for all of them, then they install one after another ---- */

#define IQ_MAX 16
enum { IQ_NONE, IQ_ASK, IQ_RUN, IQ_DONE };
enum { IQR_WAIT, IQR_OK, IQR_FAILED, IQR_CANCELLED };

typedef struct {
    char     path[PATH_CAP];
    vpk_info info;
    int      res;                    /* IQR_* */
    char     msg[96];
} iq_item;

static iq_item  s_iq[IQ_MAX];
int      s_iq_n, s_iq_pos, s_iq_phase;
static int      s_iq_skipped;            /* packages that cannot be installed and were left out */
static char     s_iq_skipped_why[100];
static bool     s_iq_auto, s_iq_over;
static uint32_t s_iq_serial;

bool iq_active(void) { return s_iq_phase != IQ_NONE; }
bool iq_running(void) { return s_iq_phase == IQ_RUN; }      /* a queue is installing right now */

/* Chimes: one when a download is finished (unless received packages are installed automatically - then it waits), and one when
 * an installation is finished (a single package, or the end of a whole queue). Nothing in between (no chime for "unpacked"). */
static uint32_t s_ch_done;

void vpk_chime_events(const vpk_status *vs)
{
    if (vs->state == VPK_DONE && vs->serial != s_ch_done) {
        s_ch_done = vs->serial;
        if (!iq_active()) chime_if_on();                 /* (a queue announces its end once, in iq_tick) */
    }
}

static bool iq_start_item(int i)
{
    if (!vpk_start(s_iq[i].path)) return false;
    vpk_status vs;
    vpk_snapshot(&vs);
    s_iq_serial = vs.serial;
    s_iq_pos = i;
    ls_strlcpy(s_inst_path, s_iq[i].path, sizeof s_inst_path);
    return true;
}

/* Called every frame: when the package that is being installed has finished, start the next one. */
void iq_tick(void)
{
    if (s_iq_phase != IQ_RUN) return;
    vpk_status vs;
    vpk_snapshot(&vs);
    vpk_chime_events(&vs);
    if (vs.serial != s_iq_serial || vs.state == VPK_RUNNING || vs.state == VPK_IDLE) return;
    iq_item *it = &s_iq[s_iq_pos];
    it->res = vs.state == VPK_DONE ? IQR_OK : (vs.state == VPK_FAILED ? IQR_FAILED : IQR_CANCELLED);
    ls_strlcpy(it->msg, vs.message, sizeof it->msg);
    ls_log("install queue: %s -> %s", it->info.title, it->res == IQR_OK ? "installed" : (it->res == IQR_FAILED ? "failed" : "cancelled"));
    if (it->res != IQR_CANCELLED) {                    /* cancelling one stops the rest; a failure does not */
        for (int next = s_iq_pos + 1; next < s_iq_n; next++) {
            if (iq_start_item(next)) return;
            s_iq[next].res = IQR_FAILED;
            ls_strlcpy(s_iq[next].msg, "Cannot start the installer", sizeof s_iq[next].msg);
        }
    }
    s_iq_phase = IQ_DONE;
    for (int i = 0; i < s_iq_n; i++)
        if (s_iq[i].res == IQR_OK) { chime_if_on(); break; }
}

static void iq_reset(void)
{
    s_iq_n = s_iq_pos = s_iq_skipped = 0;
    s_iq_phase = IQ_NONE;
    s_iq_over = false;
    s_iq_skipped_why[0] = 0;
}

/* Looks inside every package; the ones that cannot be installed are left out. One package left over is handled like
 * a normal install (no queue); `autostart` skips the question. */
void iq_open(const char *const *paths, int n, screen_id ret, bool autostart)
{
    if (xfer_busy()) { toast("Wait until the transfer is finished"); return; }
    if (vpk_busy()) { s_inst_asking = false; s_iq_phase = IQ_NONE; s_inst_return = ret; go(SC_INSTALL); return; }
    iq_reset();
    for (int i = 0; i < n; i++) {
        vpk_info in;
        memset(&in, 0, sizeof in);
        vpk_inspect(paths[i], &in);
        if (!in.ok) {
            if (!s_iq_skipped) snprintf(s_iq_skipped_why, sizeof s_iq_skipped_why, "%.40s: %.50s", base_name(paths[i]), in.err);
            s_iq_skipped++;
            continue;
        }
        if (s_iq_n >= IQ_MAX) { s_iq_over = true; continue; }
        ls_strlcpy(s_iq[s_iq_n].path, paths[i], PATH_CAP);
        s_iq[s_iq_n].info = in;
        s_iq[s_iq_n].res = IQR_WAIT;
        s_iq[s_iq_n].msg[0] = 0;
        s_iq_n++;
    }
    s_iq_auto = autostart;
    if (s_iq_n == 0) {
        toast("%s", s_iq_skipped ? s_iq_skipped_why : "Nothing to install");
        iq_reset();
        return;
    }
    if (s_iq_over) toast("Only the first %d packages are installed in one go", IQ_MAX);
    if (s_iq_n == 1) {                                  /* a single package: the normal dialog */
        char one[PATH_CAP];
        ls_strlcpy(one, s_iq[0].path, sizeof one);
        int skipped = s_iq_skipped;
        iq_reset();
        install_open(one, ret);
        if (skipped) toast("%d other package%s cannot be installed", skipped, skipped == 1 ? "" : "s");
        if (autostart && s_inst_asking && s_inst_info.ok && vpk_start(s_inst_path)) { s_inst_asking = false; s_inst_auto = true; }
        return;
    }
    s_inst_return = ret;
    s_inst_asking = false;
    s_inst_auto = false;
    if (autostart) {
        s_iq_phase = IQ_RUN;
        if (!iq_start_item(0)) { s_iq[0].res = IQR_FAILED; ls_strlcpy(s_iq[0].msg, "Cannot start the installer", sizeof s_iq[0].msg); s_iq_phase = IQ_DONE; }
    } else {
        s_iq_phase = IQ_ASK;
    }
    go(SC_INSTALL);
}

static void iq_draw(const xfer_status *x)
{
    int dx, dy;
    ui_dialog_begin(SC_INSTALL | (A.tab << 8) | (s_iq_phase << 16), 700, 380, under_tab, (void *)x, &dx, &dy);
    int lx = dx + 32, w = 700 - 64;
    char line[300];

    if (s_iq_phase == IQ_ASK) {
        gfx_circle((float)(lx + 28), (float)(dy + 52), 28, C_ACCENT_BG);
        gfx_icon(IC_PACKAGE, (float)(lx + 28), (float)(dy + 52), 30, C_ACCENT_HI);
        gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, "Install these apps?");
        snprintf(line, sizeof line, "%d packages, one after another - you only need to confirm once.", s_iq_n);
        gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, line);
        int shown = s_iq_n > 6 ? 5 : s_iq_n, unsafe = 0;
        for (int i = 0; i < s_iq_n; i++) if (s_iq[i].info.unsafe) unsafe++;
        for (int i = 0; i < shown; i++) {
            const vpk_info *in = &s_iq[i].info;
            snprintf(line, sizeof line, "%s  \xC2\xB7  %s%s", in->title, in->version[0] ? in->version : "?", in->unsafe ? "  \xC2\xB7  extended permissions" : "");
            gfx_text_fit(lx, dy + 112 + i * 26, w, 16, GW_MED, in->unsafe ? C_WARN : C_TEXT, GA_LEFT, line);
        }
        if (s_iq_n > shown) {
            snprintf(line, sizeof line, "+ %d more", s_iq_n - shown);
            gfx_text(lx, dy + 112 + shown * 26, 16, GW_REG, C_DIM, GA_LEFT, line);
        }
        int ny = dy + 276;
        if (s_iq_skipped) {
            snprintf(line, sizeof line, "%d package%s cannot be installed and %s left out (%s)", s_iq_skipped, s_iq_skipped == 1 ? "" : "s", s_iq_skipped == 1 ? "is" : "are", s_iq_skipped_why);
            gfx_text_fit(lx, ny, w, 13, GW_REG, C_BAD, GA_LEFT, line);
            ny += 22;
        }
        if (unsafe) gfx_text_fit(lx, ny, w, 13, GW_REG, C_WARN, GA_LEFT, "Some ask for extended permissions. Only install apps from sources you trust.");
        snprintf(line, sizeof line, "Install all %d", s_iq_n);
        int w1 = ui_action_width(PB_CROSS, line);
        ui_action(lx, dy + 330, PB_CROSS, line);
        ui_action(lx + w1 + 10, dy + 330, PB_CIRCLE, "Cancel");
        ui_hints("X Install all|O Cancel");
        return;
    }

    if (s_iq_phase == IQ_RUN) {
        vpk_status vs;
        vpk_snapshot(&vs);
        gfx_circle((float)(lx + 28), (float)(dy + 52), 28, C_ACCENT_BG);
        gfx_icon(IC_PACKAGE, (float)(lx + 28), (float)(dy + 52), 30, C_ACCENT_HI);
        snprintf(line, sizeof line, "Installing %d of %d", s_iq_pos + 1, s_iq_n);
        gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, line);
        gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, s_iq[s_iq_pos].info.title);
        gfx_text_fit(lx, dy + 128, w, 17, GW_MED, C_TEXT, GA_LEFT, vs.stage);
        ui_progress(lx, dy + 184, w - 70, 12, (uint64_t)vs.pct, 100, C_ACCENT);
        snprintf(line, sizeof line, "%d %%", vs.pct);
        gfx_text(lx + w, dy + 190, 17, GW_MED, C_TEXT, GA_RIGHT, line);
        if (s_iq_pos + 1 < s_iq_n) {
            snprintf(line, sizeof line, "Next: %s", s_iq[s_iq_pos + 1].info.title);
            gfx_text_fit(lx, dy + 232, w, 14, GW_REG, C_DIM, GA_LEFT, line);
        }
        if (s_iq_auto) gfx_text_fit(lx, dy + 262, w, 13, GW_REG, C_FAINT, GA_LEFT, "Started automatically - Settings > Install received .vpk files");
        if (vs.pct < 92) {
            ui_action(lx, dy + 330, PB_CIRCLE, "Cancel (stops the rest)");
            ui_hints("O Cancel (stops the rest)");
        } else {
            gfx_text(lx, dy + 346, 15, GW_REG, C_DIM, GA_LEFT, "Almost done - please keep the Vita on.");
            ui_hints("");
        }
        return;
    }

    /* IQ_DONE: what happened to each package */
    int ok = 0;
    for (int i = 0; i < s_iq_n; i++) if (s_iq[i].res == IQR_OK) ok++;
    bool all = ok == s_iq_n;
    gfx_circle((float)(lx + 28), (float)(dy + 52), 28, all ? GRGBA(102, 187, 106, 40) : GRGBA(255, 193, 77, 40));
    gfx_icon(all ? IC_CHECK : IC_CLOSE, (float)(lx + 28), (float)(dy + 52), 30, all ? C_GOOD : C_WARN);
    gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, all ? "All installed" : "Finished with problems");
    snprintf(line, sizeof line, "%d of %d installed", ok, s_iq_n);
    gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, line);
    int shown = s_iq_n > 8 ? 7 : s_iq_n;
    for (int i = 0; i < shown; i++) {
        const iq_item *it = &s_iq[i];
        gcol c = it->res == IQR_OK ? C_GOOD : (it->res == IQR_FAILED ? C_BAD : (it->res == IQR_CANCELLED ? C_WARN : C_DIM));
        gfx_icon(it->res == IQR_OK ? IC_CHECK : IC_CLOSE, (float)(lx + 10), (float)(dy + 120 + i * 26), 16, c);
        if (it->res == IQR_OK) snprintf(line, sizeof line, "%s", it->info.title);
        else if (it->res == IQR_WAIT) snprintf(line, sizeof line, "%s  \xC2\xB7  not installed", it->info.title);
        else snprintf(line, sizeof line, "%s  \xC2\xB7  %s", it->info.title, it->res == IQR_CANCELLED ? "cancelled" : it->msg);
        gfx_text_fit(lx + 30, dy + 120 + i * 26, w - 30, 15, GW_REG, it->res == IQR_OK ? C_TEXT : c, GA_LEFT, line);
    }
    if (s_iq_n > shown) {
        snprintf(line, sizeof line, "+ %d more", s_iq_n - shown);
        gfx_text(lx + 30, dy + 120 + shown * 26, 15, GW_REG, C_DIM, GA_LEFT, line);
    }
    int w1 = ui_action_width(PB_CROSS, "Close");
    ui_action(lx, dy + 330, PB_CROSS, "Close");
    const iq_item *only = NULL;                         /* exactly one package installed: it can be opened */
    for (int i = 0; i < s_iq_n; i++) if (s_iq[i].res == IQR_OK) only = ok == 1 ? &s_iq[i] : NULL;
    if (only && plat_can_launch()) {
        snprintf(line, sizeof line, "Open %.40s", only->info.title);
        ui_action(lx + w1 + 10, dy + 330, PB_SQUARE, line);
        ui_hints("X Close|SQ Open the app");
    } else {
        ui_hints("X Close");
    }
}

static void iq_input(uint32_t p)
{
    if (s_iq_phase == IQ_ASK) {
        if (p & UI_BTN_CIRCLE) { iq_reset(); go(s_inst_return); }
        if (p & UI_BTN_CROSS) {
            s_iq_phase = IQ_RUN;
            if (!iq_start_item(0)) { s_iq_phase = IQ_ASK; toast("Cannot start the installer"); }
        }
        return;
    }
    if (s_iq_phase == IQ_RUN) {
        vpk_status vs;
        vpk_snapshot(&vs);
        if ((p & UI_BTN_CIRCLE) && vs.state == VPK_RUNNING && vs.pct < 92) vpk_cancel();
        return;
    }
    if ((p & UI_BTN_SQUARE) && plat_can_launch()) {
        int ok = 0, idx = -1;
        for (int i = 0; i < s_iq_n; i++) if (s_iq[i].res == IQR_OK) { ok++; idx = i; }
        if (ok == 1) {
            if (!plat_launch_app(s_iq[idx].info.titleid)) toast("Cannot open the app from here - use its LiveArea bubble");
            return;
        }
    }
    if (p & (UI_BTN_CROSS | UI_BTN_CIRCLE)) { iq_reset(); go(s_inst_return); }
}

