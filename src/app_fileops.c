/*
 * app_fileops.c - two jobs of the Files tab that need a progress dialog: deleting files and folders, and unzipping a .zip file
 *
 * Deleting runs in small steps from the main loop (delete_tick, about 30 ms of work per call), like copying does in app_move.c, so
 * the screen keeps moving and O stops it (what is already deleted stays deleted). A folder is walked with an explicit stack of
 * directory listings.
 *
 * Unzipping is unzip.c (a thread); this file asks first ("Unzip this file?"), shows the progress and the result.
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"
#include "unzip.h"

/* ============================================================== deleting */

#define DEL_DEPTH 16

typedef struct {
    char         path[PATH_CAP];
    plat_dirent *ent;
    int          n, i;
} del_frame;

static struct {
    bool      active, cancel;
    char      path[MAX_SEL][PATH_CAP];
    int       n, cur;
    int       ok, bad;
    char      first_bad[96];
    uint64_t  total, done;                 /* items (files and folders) */
    del_frame st[DEL_DEPTH];
    int       depth;
    bool      item_active, item_bad;
} D;

bool delete_busy(void) { return D.active; }

static void count_items(const char *path, int depth, uint64_t *n)
{
    bool is_dir = false;
    if (!plat_path_info(path, &is_dir, NULL)) return;
    (*n)++;
    if (!is_dir || depth >= DEL_DEPTH) return;
    plat_dirent *v = NULL;
    int c = plat_list_dir(path, &v);
    for (int i = 0; i < c; i++) {
        char sub[PATH_CAP];
        if (snprintf(sub, sizeof sub, "%s/%s", path, v[i].name) >= (int)sizeof sub) continue;
        count_items(sub, depth + 1, n);
    }
    free(v);
}

bool delete_start(char list[][PATH_CAP], int n)
{
    if (D.active || n <= 0) return false;
    D.n = n > MAX_SEL ? MAX_SEL : n;
    D.total = D.done = 0;
    for (int i = 0; i < D.n; i++) {
        ls_strlcpy(D.path[i], list[i], PATH_CAP);
        count_items(D.path[i], 0, &D.total);
    }
    D.cur = D.ok = D.bad = D.depth = 0;
    D.first_bad[0] = 0;
    D.item_active = D.item_bad = D.cancel = false;
    D.active = true;
    ls_log("delete: %d item(s), %llu entries", D.n, (unsigned long long)D.total);
    return true;
}

static void free_frames(void)
{
    while (D.depth > 0) free(D.st[--D.depth].ent);
}

static void end_item(void)
{
    free_frames();
    if (D.item_bad) {
        D.bad++;
        if (!D.first_bad[0]) ls_strlcpy(D.first_bad, base_name(D.path[D.cur]), sizeof D.first_bad);
    } else {
        D.ok++;
    }
    D.cur++;
    D.item_active = false;
}

static bool push_frame(const char *path)
{
    if (D.depth >= DEL_DEPTH) return false;
    del_frame *f = &D.st[D.depth];
    f->ent = NULL;
    f->n = plat_list_dir(path, &f->ent);
    if (f->n < 0) return false;
    f->i = 0;
    ls_strlcpy(f->path, path, sizeof f->path);
    D.depth++;
    return true;
}

static void slow_down(void)
{
#ifdef LS_HOST
    static int slow = -1;
    if (slow < 0) { const char *e = getenv("LS_SLOW_DELETE_MS"); slow = e ? atoi(e) : 0; }
    if (slow > 0) plat_sleep_ms((unsigned)slow);        /* tests: slow enough to be cancelled */
#endif
}

static void start_item(void)
{
    const char *p = D.path[D.cur];
    bool is_dir = false;
    if (!plat_path_info(p, &is_dir, NULL)) { D.ok++; D.cur++; return; }           /* already gone */
    D.item_active = true;
    D.item_bad = false;
    D.depth = 0;
    if (!is_dir) {
        slow_down();
        if (!plat_remove(p)) D.item_bad = true;
        D.done++;
        end_item();
    } else if (!push_frame(p)) {
        D.item_bad = true;
        end_item();
    }
}

static void walk_step(void)
{
    del_frame *f = &D.st[D.depth - 1];
    if (f->i >= f->n) {                                  /* everything inside is gone: the folder itself */
        slow_down();
        if (!plat_rmdir(f->path)) D.item_bad = true;
        D.done++;
        free(f->ent);
        f->ent = NULL;
        D.depth--;
        if (D.depth == 0) end_item();
        return;
    }
    const plat_dirent *e = &f->ent[f->i++];
    char s[PATH_CAP];
    if (snprintf(s, sizeof s, "%s/%s", f->path, e->name) >= (int)sizeof s) { D.item_bad = true; return; }
    if (e->is_dir) {
        if (!push_frame(s)) D.item_bad = true;
    } else {
        slow_down();
        if (!plat_remove(s)) D.item_bad = true;
        D.done++;
    }
}

static void delete_finish(bool cancelled)
{
    if (D.item_active) { free_frames(); D.item_active = false; }      /* (cancelled in the middle of an item) */
    D.active = false;
    int ok = D.ok, bad = D.bad;
    char first[96];
    ls_strlcpy(first, D.first_bad, sizeof first);
    ls_log("delete: %d done, %d failed%s", ok, bad, cancelled ? " (cancelled)" : "");
    files_delete_finished(ok, bad, first, cancelled);
}

void delete_tick(void)
{
    if (!D.active) return;
    uint64_t t0 = plat_time_ms();
    do {
        if (D.cancel) { delete_finish(true); return; }
        if (!D.item_active) {
            if (D.cur >= D.n) { delete_finish(false); return; }
            start_item();
        } else {
            walk_step();
        }
    } while (plat_time_ms() - t0 < 30);
}

void delete_draw(const xfer_status *x)
{
    int dx, dy;
    ui_dialog_begin(SC_DELETE | (A.tab << 8), 640, 250, under_tab, (void *)x, &dx, &dy);
    int lx = dx + 32, w = 640 - 64;
    gfx_circle((float)(lx + 28), (float)(dy + 52), 28, GRGBA(239, 83, 80, 40));
    gfx_icon(IC_CLOSE, (float)(lx + 28), (float)(dy + 52), 30, C_BAD);
    gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, "Deleting");
    gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, D.cur < D.n ? base_name(D.path[D.cur]) : "");

    char line[120];
    uint64_t total = D.total ? D.total : 1, done = D.done > total ? total : D.done;
    snprintf(line, sizeof line, "%llu of %llu files and folders", (unsigned long long)done, (unsigned long long)D.total);
    gfx_text_fit(lx, dy + 120, w, 17, GW_MED, C_TEXT, GA_LEFT, line);
    ui_progress(lx, dy + 150, w - 70, 12, done, total, C_BAD);
    char pcs[16];
    snprintf(pcs, sizeof pcs, "%d %%", (int)(done * 100 / total));
    gfx_text(lx + w, dy + 156, 17, GW_MED, C_TEXT, GA_RIGHT, pcs);

    ui_action(lx, dy + 200, PB_CIRCLE, "Stop");
    ui_hints("O Stop");
}

void delete_input(uint32_t p)
{
    if (p & UI_BTN_CIRCLE) D.cancel = true;
}

/* ============================================================== unzipping */

static char       s_uz_path[PATH_CAP], s_uz_dest[PATH_CAP], s_uz_name[PATH_CAP];
static unzip_info s_uz_info;
static bool       s_uz_asking;
static screen_id  s_uz_ret;
static uint32_t   s_uz_seen;                              /* the status serial whose result has been shown */

bool unzip_ui_busy(void) { return unzip_busy(); }

void unzip_ui_open(const char *path, screen_id ret)
{
    s_uz_ret = ret;
    if (unzip_busy()) { s_uz_asking = false; go(SC_UNZIP); return; }
    ls_strlcpy(s_uz_path, path, sizeof s_uz_path);
    unzip_inspect(path, &s_uz_info);
    /* into a new folder next to the file, named like it ("photos.zip" -> "photos"); an existing name gets "(1)" */
    char dest[PATH_CAP];
    ls_strlcpy(dest, path, sizeof dest);
    size_t n = strlen(dest);
    if (n > 4) dest[n - 4] = 0;
    if (!unique_path(dest, sizeof dest, true)) s_uz_info.ok = false, snprintf(s_uz_info.err, sizeof s_uz_info.err, "No free folder name here");
    ls_strlcpy(s_uz_dest, dest, sizeof s_uz_dest);
    ls_strlcpy(s_uz_name, base_name(dest), sizeof s_uz_name);
    s_uz_asking = true;
    unzip_status us;
    unzip_snapshot(&us);
    s_uz_seen = us.serial;
    go(SC_UNZIP);
}

/* Main loop: a result that arrived while another screen was on top (a request popped up, say) is reported as a message. */
void unzip_ui_tick(void)
{
    unzip_status us;
    unzip_snapshot(&us);
    if (s_uz_asking || us.serial == s_uz_seen || us.state == UZ_RUNNING || us.state == UZ_IDLE) return;
    if (A.sc == SC_UNZIP) return;
    s_uz_seen = us.serial;
    files_reload();
    if (us.state == UZ_DONE) toast("Unzipped into %.60s", s_uz_name);
    else toast("%.100s", us.message);
}

void unzip_ui_draw(const xfer_status *x)
{
    unzip_status us;
    unzip_snapshot(&us);
    int dx, dy;
    ui_dialog_begin(SC_UNZIP | (A.tab << 8), 700, 320, under_tab, (void *)x, &dx, &dy);
    int lx = dx + 32, w = 700 - 64;
    char line[260], sz[24];
    gcol tint = C_ACCENT_BG, icol = C_ACCENT_HI;
    int icon = IC_FOLDER;
    const char *title = "Unzip this file?";
    bool running = !s_uz_asking && us.state == UZ_RUNNING;
    bool ok_done = !s_uz_asking && us.state == UZ_DONE;
    bool bad = (s_uz_asking && !s_uz_info.ok) || (!s_uz_asking && (us.state == UZ_FAILED || us.state == UZ_CANCELLED));
    if (running) title = "Unzipping";
    else if (ok_done) { title = "Unzipped"; icon = IC_CHECK; icol = C_GOOD; tint = GRGBA(102, 187, 106, 40); }
    else if (bad) { title = us.state == UZ_CANCELLED && !s_uz_asking ? "Cancelled" : "Cannot unzip this file"; icon = IC_CLOSE; icol = C_BAD; tint = GRGBA(239, 83, 80, 40); }
    gfx_circle((float)(lx + 28), (float)(dy + 52), 28, tint);
    gfx_icon(icon, (float)(lx + 28), (float)(dy + 52), 30, icol);
    gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, title);
    gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, base_name(s_uz_path));

    if (s_uz_asking && s_uz_info.ok) {
        fmt_size(s_uz_info.unpacked, sz, sizeof sz);
        snprintf(line, sizeof line, "%d file%s  \xC2\xB7  %s unpacked", s_uz_info.files, s_uz_info.files == 1 ? "" : "s", sz);
        gfx_text_fit(lx, dy + 128, w, 17, GW_MED, C_TEXT, GA_LEFT, line);
        snprintf(line, sizeof line, "Into a new folder: %.120s", s_uz_name);
        gfx_text_fit(lx, dy + 160, w, 15, GW_REG, C_DIM, GA_LEFT, line);
        gfx_text_fit(lx, dy + 186, w, 15, GW_REG, C_DIM, GA_LEFT, "The zip file itself stays as it is.");
        int w1 = ui_action_width(PB_CROSS, "Unzip");
        ui_action(lx, dy + 250, PB_CROSS, "Unzip");
        ui_action(lx + w1 + 10, dy + 250, PB_CIRCLE, "Cancel");
        ui_hints("X Unzip|O Cancel");
    } else if (s_uz_asking) {
        gfx_text_fit(lx, dy + 128, w, 16, GW_MED, C_BAD, GA_LEFT, s_uz_info.err);
        ui_action(lx, dy + 250, PB_CROSS, "Close");
        ui_hints("X Close");
    } else if (running) {
        snprintf(line, sizeof line, "%d / %d  \xC2\xB7  %s", us.files_done < us.files_total ? us.files_done + 1 : us.files_total, us.files_total, us.cur);
        gfx_text_fit(lx, dy + 124, w, 17, GW_MED, C_TEXT, GA_LEFT, line);
        uint64_t total = us.total ? us.total : 1, done = us.done > total ? total : us.done;
        ui_progress(lx, dy + 160, w - 70, 12, done, total, C_ACCENT);
        snprintf(line, sizeof line, "%d %%", (int)(done * 100 / total));
        gfx_text(lx + w, dy + 166, 17, GW_MED, C_TEXT, GA_RIGHT, line);
        gfx_text_fit(lx, dy + 196, w, 13, GW_REG, C_DIM, GA_LEFT, "If you stop, the new folder is removed again.");
        ui_action(lx, dy + 250, PB_CIRCLE, "Cancel");
        ui_hints("O Cancel");
    } else if (ok_done) {
        snprintf(line, sizeof line, "%d files are in the folder %.120s.", us.files_total, s_uz_name);
        if (us.files_total == 1) snprintf(line, sizeof line, "1 file is in the folder %.120s.", s_uz_name);
        gfx_text_fit(lx, dy + 128, w, 17, GW_MED, C_TEXT, GA_LEFT, line);
        ui_action(lx, dy + 250, PB_CROSS, "Show the folder");
        ui_hints("X Show the folder");
    } else {
        gfx_text_fit(lx, dy + 128, w, 16, GW_MED, C_BAD, GA_LEFT, us.message);
        ui_action(lx, dy + 250, PB_CROSS, "Close");
        ui_hints("X Close");
    }
}

void unzip_ui_input(uint32_t p)
{
    unzip_status us;
    unzip_snapshot(&us);
    if (s_uz_asking) {
        if (!s_uz_info.ok) { if (p & (UI_BTN_CROSS | UI_BTN_CIRCLE)) go(s_uz_ret); return; }
        if (p & UI_BTN_CIRCLE) { go(s_uz_ret); return; }
        if (p & UI_BTN_CROSS) {
            if (unzip_start(s_uz_path, s_uz_dest)) { s_uz_asking = false; ls_log("unzip: %s -> %s", s_uz_path, s_uz_dest); }
            else toast("Cannot start unzipping");
        }
        return;
    }
    if (us.state == UZ_RUNNING) {
        if (p & UI_BTN_CIRCLE) unzip_cancel();
        return;
    }
    if (p & (UI_BTN_CROSS | UI_BTN_CIRCLE)) {
        s_uz_seen = us.serial;
        files_ensure();
        files_reload();
        if (us.state == UZ_DONE) files_focus_name(s_uz_name);
        go(s_uz_ret);
    }
}
