/*
 * app_files.c - the file browser (choose what to send) and the Files tab with its options menu
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"
#include "unzip.h"

/* ---- state of the file browser and the Files tab ---- */
static browser_mode s_bmode;
static fbrowser     s_sb;                    /* the "choose files to send" / "choose folder" page */
static fbrowser     s_fb;                    /* the Files tab (keeps its place while you use other tabs) */
static bool         s_fb_ready;
static char         s_mv[MAX_SEL][PATH_CAP];      /* items marked with "Move" or "Copy", waiting for a destination folder */
static int          s_nmv;
static bool         s_mv_copy;                    /* the marked items are to be copied (otherwise moved) */
static int          s_pre_moved, s_pre_skipped;   /* of the same "Move here": renamed at once / left alone (for the final message) */
char         s_sel[MAX_SEL][PATH_CAP];       /* what is ticked: the send selection (shared with the Send page) */
int          s_nsel;


static void path_join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (l && dir[l - 1] == '/') snprintf(out, cap, "%s%s", dir, name);
    else snprintf(out, cap, "%s/%s", dir, name);
}

/* "ux0:/data/x" -> "ux0:/data"; "ux0:" -> "" (list of devices); "/home" -> "/"; "/" -> "" */
static void path_parent(char *path)
{
    char *slash = strrchr(path, '/');
    if (!slash) { path[0] = 0; return; }
    if (slash == path) {
        if (path[1]) path[1] = 0;
        else path[0] = 0;
        return;
    }
    *slash = 0;
}

static int cmp_entries(const void *a, const void *b)
{
    const plat_dirent *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return ls_strcasecmp(x->name, y->name);
}

static bool is_selected(const char *path)
{
    for (int i = 0; i < s_nsel; i++)
        if (!strcmp(s_sel[i], path)) return true;
    return false;
}

static bool is_moving(const char *path)
{
    for (int i = 0; i < s_nmv; i++)
        if (!strcmp(s_mv[i], path)) return true;
    return false;
}

static void toggle_selected(const char *path)
{
    for (int i = 0; i < s_nsel; i++) {
        if (!strcmp(s_sel[i], path)) {
            for (int k = i; k + 1 < s_nsel; k++) memcpy(s_sel[k], s_sel[k + 1], PATH_CAP);
            s_nsel--;
            return;
        }
    }
    if (s_nsel >= MAX_SEL) { toast("You can select at most %d items at once", MAX_SEL); return; }
    ls_strlcpy(s_sel[s_nsel++], path, PATH_CAP);
}

/* The round tick box in front of a file browser row; `rec_x` is where the dump records it. */
static void row_tick(int cx, int cy, int rec_x, bool on)
{
    if (on) {
        gfx_circle((float)cx, (float)cy, 10, C_ACCENT);
        gfx_icon(IC_CHECK, (float)cx, (float)cy, 16, C_ON_ACCENT);
        gfx_record(rec_x, cy, "[x]");
    } else {
        gfx_ring((float)cx, (float)cy, 9, 1.8f, C_FAINT);
        gfx_record(rec_x, cy, "[ ]");
    }
}

/* ============================================================== file browser */

static void browser_load(fbrowser *b)
{
    free(b->ent);
    b->ent = NULL;
    b->nent = 0;
    if (!b->path[0]) {
        char roots[8][16];
        int n = plat_list_roots(roots, 8);
        b->ent = calloc((size_t)(n > 0 ? n : 1), sizeof *b->ent);
        for (int i = 0; i < n && b->ent; i++) {
            ls_strlcpy(b->ent[i].name, roots[i], sizeof b->ent[i].name);
            b->ent[i].is_dir = true;
        }
        b->nent = b->ent ? n : 0;
    } else {
        b->nent = plat_list_dir(b->path, &b->ent);
        if (b->nent < 0) {
            toast("Cannot open this folder");
            b->nent = 0;
            path_parent(b->path);
            browser_load(b);
            return;
        }
        qsort(b->ent, (size_t)b->nent, sizeof *b->ent, cmp_entries);
    }
    memset(&b->cur, 0, sizeof b->cur);
    b->free_bytes = b->path[0] ? plat_free_space(b->path) : UINT64_MAX;
}

void browser_open(browser_mode mode)
{
    fbrowser *b = &s_sb;
    s_bmode = mode;
    const char *start = plat_browse_start();
    b->path[0] = 0;
    if (start && *start) ls_strlcpy(b->path, start, sizeof b->path);
    if (mode == BM_SEND) s_nsel = 0;
    browser_load(b);
    go(SC_BROWSER);
}

static void browser_entry_path(const fbrowser *b, int idx, char *out, size_t cap)
{
    if (!b->path[0]) ls_strlcpy(out, b->ent[idx].name, cap);
    else path_join(out, cap, b->path, b->ent[idx].name);
}

#define BROWSER_TOP  130
#define BROWSER_ROWS 9
#define BROWSER_STEP 38

void browser_draw(const xfer_status *x)
{
    fbrowser *b = &s_sb;
    draw_chrome(s_bmode == BM_SEND ? TAB_SEND : TAB_SETTINGS, x);
    ui_page_title(s_bmode == BM_SEND ? "Choose files or folders to send" : "Choose the folder for received files", NULL);

    ui_card(CONTENT_X, 80, CONTENT_W, 40, false);
    gfx_icon(IC_FOLDER, CONTENT_X + 26, 100, 22, C_ACCENT_HI);
    char cnt[32] = "";
    int right = CONTENT_R - 18;
    if (b->path[0]) {                                              /* touch: go up one folder */
        int uw = ui_action_width(PB_CIRCLE, "Up");
        ui_action(CONTENT_R - 8 - uw, 100, PB_CIRCLE, "Up");
        right -= uw - 4;
    }
    if (b->nent > BROWSER_ROWS) {
        snprintf(cnt, sizeof cnt, "%d / %d", b->cur.sel + 1, b->nent);
        right -= gfx_text(right, 100, 14, GW_REG, C_DIM, GA_RIGHT, cnt) + 14;
    }
    if (s_bmode == BM_SEND && s_nsel > 0) {
        char sel[32];
        snprintf(sel, sizeof sel, "%d selected", s_nsel);
        right -= ui_chip(right, 100, sel, C_ACCENT_HI) + 10;
    }
    gfx_text_fit(CONTENT_X + 50, 100, right - CONTENT_X - 60, 16, GW_MED, C_TEXT, GA_LEFT, b->path[0] ? b->path : "Storage devices");

    ui_cursor_clamp(&b->cur, b->nent, BROWSER_ROWS);
    if (b->nent == 0) gfx_text(CONTENT_X + 24, BROWSER_TOP + 20, 16, GW_REG, C_DIM, GA_LEFT, "(empty)");
    for (int i = 0; i < BROWSER_ROWS && b->cur.top + i < b->nent; i++) {
        int idx = b->cur.top + i;
        int y = BROWSER_TOP + i * BROWSER_STEP, cy = y + 17;
        const plat_dirent *e = &b->ent[idx];
        char full[PATH_CAP];
        browser_entry_path(b, idx, full, sizeof full);
        bool sel = s_bmode == BM_SEND && is_selected(full);
        bool cur = idx == b->cur.sel;
        ui_hit_row(CONTENT_X, y - 2, CONTENT_W - 14, BROWSER_STEP, &b->cur, idx, UI_BTN_CROSS);
        if (s_bmode == BM_SEND) ui_hit_row(CONTENT_X, y - 2, 56, BROWSER_STEP, &b->cur, idx, UI_BTN_SQUARE);   /* the tick circle */
        if (cur) {
            gfx_rrect(CONTENT_X, y, CONTENT_W - 14, 34, 10, C_ACCENT_BG);
            gfx_rrect(CONTENT_X, y + 7, 4, 20, 2, C_ACCENT);
            gfx_focus(true);
        }
        int tx = CONTENT_X + 22;
        if (s_bmode == BM_SEND) {
            row_tick(CONTENT_X + 32, cy, CONTENT_X + 22, sel);
            tx = CONTENT_X + 54;
        }
        gfx_icon(e->is_dir ? IC_FOLDER : IC_FILE, (float)(tx + 12), (float)cy, 22, e->is_dir ? C_ACCENT_HI : C_DIM);
        char label[260];
        snprintf(label, sizeof label, "%s%s", e->name, e->is_dir && b->path[0] ? "/" : "");
        gfx_text_fit(tx + 34, cy, CONTENT_W - (tx - CONTENT_X) - 34 - 120, 16, e->is_dir ? GW_MED : GW_REG, C_TEXT, GA_LEFT, label);
        if (!e->is_dir) {
            char sz[24];
            fmt_size(e->size, sz, sizeof sz);
            gfx_text(CONTENT_R - 34, cy, 14, GW_REG, C_DIM, GA_RIGHT, sz);
        }
        gfx_focus(false);
    }
    ui_scrollbar(CONTENT_R - 6, BROWSER_TOP, BROWSER_ROWS * BROWSER_STEP - 4, b->cur.top, BROWSER_ROWS, b->nent);
    ui_hit_scroll(CONTENT_X, BROWSER_TOP, CONTENT_W, BROWSER_ROWS * BROWSER_STEP, &b->cur.top, &b->cur.sel, b->nent, BROWSER_ROWS, BROWSER_STEP);

    if (s_bmode == BM_SEND) ui_hints("DUD Move|X Open / select|SQ Select|O Up|TRI Done");
    else ui_hints("DUD Move|X Open|O Up|TRI Use this folder");
}

void browser_input(uint32_t p)
{
    fbrowser *b = &s_sb;
    if (p & UI_BTN_UP)   ui_cursor_move(&b->cur, -1, b->nent, BROWSER_ROWS);
    if (p & UI_BTN_DOWN) ui_cursor_move(&b->cur, 1, b->nent, BROWSER_ROWS);
    if (p & UI_BTN_L)    ui_cursor_move(&b->cur, -BROWSER_ROWS, b->nent, BROWSER_ROWS);
    if (p & UI_BTN_R)    ui_cursor_move(&b->cur, BROWSER_ROWS, b->nent, BROWSER_ROWS);

    char full[PATH_CAP] = "";
    bool have = b->nent > 0 && b->cur.sel < b->nent;
    if (have) browser_entry_path(b, b->cur.sel, full, sizeof full);

    if ((p & UI_BTN_CROSS) && have) {
        if (b->ent[b->cur.sel].is_dir) {
            ls_strlcpy(b->path, full, sizeof b->path);
            browser_load(b);
        } else if (s_bmode == BM_SEND) {
            toggle_selected(full);
        }
    }
    if ((p & UI_BTN_SQUARE) && have && s_bmode == BM_SEND) toggle_selected(full);
    if (p & UI_BTN_CIRCLE) {
        if (!b->path[0]) {
            go(s_bmode == BM_SEND ? SC_SEND : SC_SETTINGS);
        } else {
            path_parent(b->path);
            browser_load(b);
        }
    }
    if (p & UI_BTN_TRIANGLE) {
        if (s_bmode == BM_PICK_FOLDER) {
            if (!b->path[0]) { toast("Open a storage device first"); return; }
            settings_set_receive_dir(b->path);
            toast("Received files will be saved in %s", b->path);
            go(SC_SETTINGS);
        } else {
            if (s_nsel == 0 && have && !b->ent[b->cur.sel].is_dir) toggle_selected(full);
            if (s_nsel == 0) { toast("Select something first: X on a file, Square on a file or folder"); return; }
            s_text_to_send[0] = 0;
            send_page_reset();
            go(SC_SEND);
        }
    }
}

/* ============================================================== files tab */

void files_ensure(void)
{
    if (s_fb_ready) return;
    s_fb_ready = true;
    ls_settings st;
    settings_snapshot(&st);
    plat_dirent *l = NULL;
    int n = st.receive_dir[0] ? plat_list_dir(st.receive_dir, &l) : -1;
    free(l);
    if (n >= 0) {
        ls_strlcpy(s_fb.path, st.receive_dir, sizeof s_fb.path);
    } else {
        const char *start = plat_browse_start();
        ls_strlcpy(s_fb.path, start ? start : "", sizeof s_fb.path);
    }
    browser_load(&s_fb);
}

/* Reads the folder again (new files may have arrived) and keeps the cursor on the same entry. */
void files_reload(void)
{
    char keep[256] = "";
    ui_cursor old = s_fb.cur;
    if (old.sel < s_fb.nent) ls_strlcpy(keep, s_fb.ent[old.sel].name, sizeof keep);
    browser_load(&s_fb);
    s_fb.cur = old;
    for (int i = 0; keep[0] && i < s_fb.nent; i++)
        if (!strcmp(s_fb.ent[i].name, keep)) { s_fb.cur.sel = i; break; }
    ui_cursor_clamp(&s_fb.cur, s_fb.nent, BROWSER_ROWS);
}

/* Touch: the tick circle of a Files row ticks it (also folders, which a tap on the row would open). */
static void files_tick_tap(void *ctx, int idx)
{
    (void)ctx;
    fbrowser *b = &s_fb;
    if (idx < 0 || idx >= b->nent) return;
    b->cur.sel = idx;
    char full[PATH_CAP];
    browser_entry_path(b, idx, full, sizeof full);
    toggle_selected(full);
}

void files_draw(const xfer_status *x)
{
    fbrowser *b = &s_fb;
    files_ensure();
    draw_chrome(TAB_FILES, x);
    char head[80] = "", fr[24] = "";
    if (b->path[0] && b->free_bytes != UINT64_MAX) fmt_size(b->free_bytes, fr, sizeof fr);
    if (b->path[0] && s_nmv > 0) {
        snprintf(head, sizeof head, "%s %d item%s%s%s", s_mv_copy ? "Copying" : "Moving", s_nmv, s_nmv == 1 ? "" : "s", fr[0] ? " \xC2\xB7 " : "", fr[0] ? fr : "");
        if (fr[0]) { size_t l = strlen(head); snprintf(head + l, sizeof head - l, " free"); }
    }
    else if (b->path[0] && s_nsel > 0 && fr[0]) snprintf(head, sizeof head, "%d selected \xC2\xB7 %s free", s_nsel, fr);
    else if (b->path[0] && s_nsel > 0) snprintf(head, sizeof head, "%d selected", s_nsel);
    else if (fr[0]) snprintf(head, sizeof head, "%s free", fr);
    ui_page_title("Files", head);

    ui_card(CONTENT_X, 80, CONTENT_W, 40, false);
    gfx_icon(IC_FOLDER, CONTENT_X + 26, 100, 22, C_ACCENT_HI);
    int right = CONTENT_R - 18;
    if (b->path[0]) {                                              /* touch: menu and go up one folder */
        int uw = ui_action_width(PB_CIRCLE, "Up"), mw = ui_action_width(PB_SELECT, "Menu");
        ui_action(CONTENT_R - 8 - uw, 100, PB_CIRCLE, "Up");
        ui_action(CONTENT_R - 8 - uw - 6 - mw, 100, PB_SELECT, "Menu");
        right -= uw + mw + 2;
        if (s_nmv > 0) {                                                  /* a move is waiting for its destination: this folder? */
            const char *vl = s_mv_copy ? "Copy here" : "Move here";
            int vw = ui_action_width(PB_TRIANGLE, vl);
            ui_action(CONTENT_R - 8 - uw - 6 - mw - 6 - vw, 100, PB_TRIANGLE, vl);
            right -= vw + 6;
        }
    }
    if (b->nent > BROWSER_ROWS) {
        char cnt[32];
        snprintf(cnt, sizeof cnt, "%d / %d", b->cur.sel + 1, b->nent);
        right -= gfx_text(right, 100, 14, GW_REG, C_DIM, GA_RIGHT, cnt) + 14;
    }
    gfx_text_fit(CONTENT_X + 50, 100, right - CONTENT_X - 60, 16, GW_MED, C_TEXT, GA_LEFT, b->path[0] ? b->path : "Storage devices");

    ui_cursor_clamp(&b->cur, b->nent, BROWSER_ROWS);
    if (b->nent == 0) gfx_text(CONTENT_X + 24, BROWSER_TOP + 20, 16, GW_REG, C_DIM, GA_LEFT, "(empty)");
    bool cur_vpk = false, cur_dir = false, cur_view = false, cur_zip = false;
    for (int i = 0; i < BROWSER_ROWS && b->cur.top + i < b->nent; i++) {
        int idx = b->cur.top + i;
        int y = BROWSER_TOP + i * BROWSER_STEP, cy = y + 17;
        const plat_dirent *e = &b->ent[idx];
        bool vpk = !e->is_dir && vpk_has_extension(e->name);
        bool zip = !e->is_dir && unzip_has_extension(e->name);
        bool cur = idx == b->cur.sel;
        ui_hit_row(CONTENT_X, y - 2, CONTENT_W - 14, BROWSER_STEP, &b->cur, idx, UI_BTN_CROSS);   /* tap: open it (a folder, a picture, a text, a package) */
        if (b->path[0]) ui_hit_call(CONTENT_X, y - 2, 56, BROWSER_STEP, files_tick_tap, NULL, idx);       /* the tick circle */
        if (cur) {
            cur_vpk = vpk;
            cur_zip = zip;
            cur_dir = e->is_dir;
            cur_view = !e->is_dir && view_kind(e->name) != 0;
            gfx_rrect(CONTENT_X, y, CONTENT_W - 14, 34, 10, C_ACCENT_BG);
            gfx_rrect(CONTENT_X, y + 7, 4, 20, 2, C_ACCENT);
            gfx_focus(true);
        }
        int tx = CONTENT_X + 22;
        if (b->path[0]) {
            char full[PATH_CAP];
            browser_entry_path(b, idx, full, sizeof full);
            row_tick(CONTENT_X + 32, cy, CONTENT_X + 22, is_selected(full));
            tx = CONTENT_X + 54;
        }
        gfx_icon(e->is_dir ? IC_FOLDER : (vpk ? IC_PACKAGE : IC_FILE), (float)(tx + 12), (float)cy, 22,
                 e->is_dir ? C_ACCENT_HI : (vpk ? C_ACCENT_HI : C_DIM));
        char label[260];
        snprintf(label, sizeof label, "%s%s", e->name, e->is_dir && b->path[0] ? "/" : "");
        int rw = (e->is_dir ? 0 : 100) + (vpk ? 70 : 0) + (zip ? 62 : 0);
        bool mv_row = false;                                              /* a row that is waiting to be moved is shown dimmed */
        if (s_nmv > 0 && b->path[0]) {
            char fp[PATH_CAP];
            browser_entry_path(b, idx, fp, sizeof fp);
            mv_row = is_moving(fp);
        }
        gfx_text_fit(tx + 34, cy, CONTENT_W - (tx - CONTENT_X) - 34 - 40 - rw, 16, e->is_dir || vpk ? GW_MED : GW_REG, mv_row ? C_FAINT : C_TEXT, GA_LEFT, label);
        int rx = CONTENT_R - 34;
        if (!e->is_dir) {
            char sz[24];
            fmt_size(e->size, sz, sizeof sz);
            rx -= gfx_text(rx, cy, 14, GW_REG, C_DIM, GA_RIGHT, sz) + 10;
        }
        if (vpk) ui_chip(rx, cy, "VPK", C_ACCENT_HI);
        else if (zip) ui_chip(rx, cy, "ZIP", C_ACCENT_HI);
        gfx_focus(false);
    }
    ui_scrollbar(CONTENT_R - 6, BROWSER_TOP, BROWSER_ROWS * BROWSER_STEP - 4, b->cur.top, BROWSER_ROWS, b->nent);
    ui_hit_scroll(CONTENT_X, BROWSER_TOP, CONTENT_W, BROWSER_ROWS * BROWSER_STEP, &b->cur.top, &b->cur.sel, b->nent, BROWSER_ROWS, BROWSER_STEP);

    /* What X does depends on what is under the cursor (and on whether several things are ticked: then it opens those). */
    const char *open_label = NULL;
    if (cur_dir) open_label = "X Open";
    else if (b->path[0] && b->nent > 0) {
        int nv = 0, nview = 0;
        for (int i = 0; s_nsel >= 2 && i < s_nsel; i++) {
            if (vpk_has_extension(s_sel[i])) nv++;
            else if (view_kind(s_sel[i])) nview++;
        }
        if (s_nsel >= 2 && nv > 0 && nview == 0) open_label = "X Install ticked";
        else if (s_nsel >= 2 && nview > 0 && nv == 0) open_label = "X View ticked";
        else if (s_nsel >= 2) open_label = "X Open ticked";
        else if (cur_vpk) open_label = "X Install";
        else if (cur_view) open_label = "X View";
        else if (cur_zip) open_label = "X Unzip";
    }
    char hints[160];
    if (!b->path[0]) snprintf(hints, sizeof hints, "DUD Move|X Open");
    else if (s_nmv > 0) snprintf(hints, sizeof hints, "DUD Move|%s%sTRI %s|SEL Menu|O Up", open_label ? open_label : "", open_label ? "|" : "", s_mv_copy ? "Copy here" : "Move here");
    else snprintf(hints, sizeof hints, "DUD Move|%s%sSQ Tick|TRI Send|SEL Menu|O Up", open_label ? open_label : "", open_label ? "|" : "");
    ui_hints(hints);
}

/* Opens the Send tab with what is ticked (or the highlighted entry when nothing is ticked). */
static void files_send(void)
{
    fbrowser *b = &s_fb;
    if (!b->path[0]) { toast("Open the storage device first"); return; }
    if (s_nsel == 0 && b->nent > 0 && b->cur.sel < b->nent) {
        char full[PATH_CAP];
        browser_entry_path(b, b->cur.sel, full, sizeof full);
        toggle_selected(full);
    }
    if (s_nsel == 0) { toast("Nothing to send here"); return; }
    s_text_to_send[0] = 0;
    send_page_reset();
    discovery_request_announce();
    go(SC_SEND);
    if (s_nsel == 1) toast("Choose who should get %s", base_name(s_sel[0]));
    else toast("Choose who should get these %d items", s_nsel);
}

static void menu_open(void);
static void move_here(void);

/* Case-insensitive order for a list of paths. */
static int cmp_paths(const void *a, const void *b)
{
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    for (; *x && *y; x++, y++) {
        int cx = *x >= 'A' && *x <= 'Z' ? *x + 32 : *x, cy = *y >= 'A' && *y <= 'Z' ? *y + 32 : *y;
        if (cx != cy) return cx - cy;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

/* X on a file. One file: a package asks "Install this app?", a picture or text opens in the viewer. Two or more ticked: the
 * ticked packages ("Install these apps?") or the ticked pictures / texts (a queue to page through). */
static void files_open_file(const char *full, bool is_vpk)
{
    const char *pk[MAX_SEL], *vw[MAX_SEL];
    int npk = 0, nvw = 0, other = 0;
    if (s_nsel >= 2) {
        for (int i = 0; i < s_nsel; i++) {
            if (vpk_has_extension(s_sel[i])) pk[npk++] = s_sel[i];
            else if (view_kind(s_sel[i])) vw[nvw++] = s_sel[i];
            else other++;
        }
        if (npk > 0 && nvw > 0) { toast("Tick only packages, or only pictures and text files"); return; }
        if (npk == 0 && nvw == 0) { toast("None of the ticked items can be opened here"); return; }
        qsort(npk ? pk : vw, (size_t)(npk ? npk : nvw), sizeof(char *), cmp_paths);
        if (other > 0) toast("%d ticked item%s cannot be opened and %s left out", other, other == 1 ? "" : "s", other == 1 ? "is" : "are");
        if (npk > 0) { iq_open(pk, npk, SC_FILES, false); return; }
        int start = 0;
        for (int i = 0; i < nvw; i++) if (!strcmp(vw[i], full)) start = i;
        view_open(vw, nvw, start, SC_FILES);
        return;
    }
    if (is_vpk) { install_open(full, SC_FILES); return; }
    if (unzip_has_extension(full)) { unzip_ui_open(full, SC_FILES); return; }
    if (view_kind(full)) { const char *one = full; view_open(&one, 1, 0, SC_FILES); return; }
    toast("This kind of file cannot be opened here - SQUARE ticks it, TRIANGLE sends it");
}

void files_input(uint32_t p)
{
    fbrowser *b = &s_fb;
    files_ensure();
    if (p & UI_BTN_UP)    ui_cursor_move(&b->cur, -1, b->nent, BROWSER_ROWS);
    if (p & UI_BTN_DOWN)  ui_cursor_move(&b->cur, 1, b->nent, BROWSER_ROWS);

    char full[PATH_CAP] = "";
    bool have = b->nent > 0 && b->cur.sel < b->nent;
    if (have) browser_entry_path(b, b->cur.sel, full, sizeof full);
    bool is_dir = have && b->ent[b->cur.sel].is_dir;
    bool is_vpk = have && !is_dir && vpk_has_extension(b->ent[b->cur.sel].name);

    if ((p & UI_BTN_CROSS) && have) {                      /* X: open - a folder, a picture / text, a .vpk; several ticked files: all of them */
        if (is_dir) {
            ls_strlcpy(b->path, full, sizeof b->path);
            browser_load(b);
        } else if (!b->path[0]) {
            toast("Open the storage device first");
        } else {
            files_open_file(full, is_vpk);
            return;
        }
    }
    if ((p & UI_BTN_SQUARE) && have && b->path[0] && s_nmv == 0) toggle_selected(full);       /* SQUARE: tick / untick */
    if (p & UI_BTN_TRIANGLE) { if (s_nmv > 0) move_here(); else files_send(); return; }
    if (p & UI_BTN_SELECT) { menu_open(); return; }
    if ((p & UI_BTN_CIRCLE) && b->path[0]) {
        char name[256];
        ls_strlcpy(name, base_name(b->path), sizeof name);
        path_parent(b->path);
        browser_load(b);
        for (int i = 0; i < b->nent; i++)
            if (!strcmp(b->ent[i].name, name)) { b->cur.sel = i; break; }
        ui_cursor_clamp(&b->cur, b->nent, BROWSER_ROWS);
    }
}

/* ---- options menu for the Files tab ---- */

enum { MI_TICKALL, MI_UNTICK, MI_RENAME, MI_DELETE, MI_NEWDIR, MI_MOVE, MI_MOVEHERE, MI_MOVECANCEL, MI_COPY, MI_UNZIP };
typedef struct { int id; char label[140]; } menu_item;
#define MENU_MAX  14                                  /* entries a menu can hold ... */
#define MENU_ROWS 9                                   /* ... and how many are visible at once (the rest scrolls) */
static menu_item s_menu[MENU_MAX];
static int       s_nmenu;
static ui_cursor s_mcur;
static char      s_op_path[PATH_CAP];                 /* the file a rename is about */
static char      s_del[MAX_SEL][PATH_CAP];     /* what a delete confirmation is about */
static int       s_ndel;

/* The question of the delete confirmation. */
void delete_question(char *q, size_t cap)
{
    if (s_ndel == 1) snprintf(q, cap, "Delete \"%.80s\"?", base_name(s_del[0]));
    else snprintf(q, cap, "Delete these %d items?", s_ndel);
}

static void menu_add(int id, const char *fmt, ...) LS_PRINTF(2, 3);
static void menu_add(int id, const char *fmt, ...)
{
    if (s_nmenu >= MENU_MAX) return;
    s_menu[s_nmenu].id = id;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_menu[s_nmenu].label, sizeof s_menu[0].label, fmt, ap);
    va_end(ap);
    s_nmenu++;
}

static void menu_open(void)
{
    fbrowser *b = &s_fb;
    files_ensure();
    if (!b->path[0]) { toast("Open a storage device first"); return; }
    bool have = b->nent > 0 && b->cur.sel < b->nent;
    const char *name = have ? b->ent[b->cur.sel].name : "";
    s_nmenu = 0;
    if (s_nmv > 0) {                                           /* a move / copy is waiting for its folder */
        menu_add(MI_MOVEHERE, "%s here (%d item%s)", s_mv_copy ? "Copy" : "Move", s_nmv, s_nmv == 1 ? "" : "s");
        menu_add(MI_MOVECANCEL, s_mv_copy ? "Cancel the copy" : "Cancel the move");
    }
    if (b->nent > 0) menu_add(MI_TICKALL, "Tick everything in this folder");
    if (s_nsel > 0) menu_add(MI_UNTICK, "Untick everything (%d)", s_nsel);
    if (s_nsel > 0) menu_add(MI_COPY, "Copy the %d ticked item%s", s_nsel, s_nsel == 1 ? "" : "s");
    else if (have) menu_add(MI_COPY, "Copy %.70s", name);
    if (s_nsel > 0) menu_add(MI_MOVE, "Move the %d ticked item%s", s_nsel, s_nsel == 1 ? "" : "s");
    else if (have) menu_add(MI_MOVE, "Move %.70s", name);
    if (have && s_nsel == 0 && !b->ent[b->cur.sel].is_dir && unzip_has_extension(name)) menu_add(MI_UNZIP, "Unzip %.70s", name);
    if (have) menu_add(MI_RENAME, "Rename %.70s", name);
    if (s_nsel > 0) menu_add(MI_DELETE, "Delete the %d ticked item%s", s_nsel, s_nsel == 1 ? "" : "s");
    else if (have) menu_add(MI_DELETE, "Delete %.70s", name);
    menu_add(MI_NEWDIR, "New folder here");
    memset(&s_mcur, 0, sizeof s_mcur);
    go(SC_MENU);
}

void menu_draw(const xfer_status *x)
{
    int vis = s_nmenu < MENU_ROWS ? s_nmenu : MENU_ROWS;
    int h = 112 + vis * 46;
    int dx, dy;
    ui_dialog_begin(SC_MENU | (A.tab << 8), 560, h, under_tab, (void *)x, &dx, &dy);
    gfx_text(dx + 32, dy + 42, 22, GW_BOLD, C_TEXT, GA_LEFT, "Options");
    gfx_text_fit(dx + 32, dy + 70, 500, 13, GW_REG, C_DIM, GA_LEFT, s_fb.path);
    ui_cursor_clamp(&s_mcur, s_nmenu, MENU_ROWS);
    for (int k = 0; k < vis; k++) {
        int i = s_mcur.top + k;
        int y = dy + 92 + k * 46;
        bool cur = i == s_mcur.sel;
        if (cur) {
            gfx_rrect(dx + 20, y, 520, 40, 10, C_ACCENT_BG);
            gfx_rrect(dx + 20, y + 8, 4, 24, 2, C_ACCENT);
            gfx_focus(true);
        }
        bool bad = s_menu[i].id == MI_DELETE;
        gfx_text_fit(dx + 42, y + 20, 480, 16, GW_MED, bad ? C_BAD : C_TEXT, GA_LEFT, s_menu[i].label);
        gfx_focus(false);
        ui_hit_row(dx + 20, y - 3, 520, 46, &s_mcur, i, UI_BTN_CROSS);
    }
    if (s_nmenu > MENU_ROWS) {
        ui_scrollbar(dx + 548, dy + 92, vis * 46 - 8, s_mcur.top, MENU_ROWS, s_nmenu);
        ui_hit_scroll(dx + 20, dy + 92, 520, vis * 46, &s_mcur.top, &s_mcur.sel, s_nmenu, MENU_ROWS, 46);
    }
    ui_hints("DUD Move|X Choose|O Close");
}

static void files_select_name(const char *name)
{
    for (int i = 0; i < s_fb.nent; i++)
        if (!strcmp(s_fb.ent[i].name, name)) { s_fb.cur.sel = i; break; }
    ui_cursor_clamp(&s_fb.cur, s_fb.nent, BROWSER_ROWS);
}

/* "ux0:data/a" covers "ux0:data/a" and everything below it */
static bool path_covers(const char *a, const char *b)
{
    size_t la = strlen(a);
    while (la > 1 && a[la - 1] == '/') la--;
    return strncmp(a, b, la) == 0 && (b[la] == 0 || b[la] == '/');
}

static void delete_ask(void)
{
    fbrowser *b = &s_fb;
    s_ndel = 0;
    if (s_nsel > 0) {
        for (int i = 0; i < s_nsel; i++) ls_strlcpy(s_del[s_ndel++], s_sel[i], PATH_CAP);
    } else if (b->nent > 0 && b->cur.sel < b->nent) {
        browser_entry_path(b, b->cur.sel, s_del[0], PATH_CAP);
        s_ndel = 1;
    }
    if (s_ndel == 0) { toast("Nothing to delete"); go(SC_FILES); return; }
    for (int i = 0; i < s_ndel; i++) {
        if (path_covers(s_del[i], plat_data_dir())) {            /* never remove the folder with our own settings and certificate */
            toast("%.60s contains VitaSend's own data - not deleted", base_name(s_del[i]));
            s_ndel = 0;
            go(SC_FILES);
            return;
        }
    }
    confirm_open(CF_DELETE, SC_FILES);
}

/* Deletes a file, or a folder with everything inside. Returns false if something could not be removed. */
bool delete_tree(const char *path, int depth)
{
    bool is_dir = false;
    if (!plat_path_info(path, &is_dir, NULL)) return true;
    if (!is_dir) return plat_remove(path);
    if (depth > 16) return false;
    plat_dirent *v = NULL;
    int n = plat_list_dir(path, &v);
    bool ok = n >= 0;
    for (int i = 0; i < n; i++) {
        char sub[600];
        snprintf(sub, sizeof sub, "%s/%s", path, v[i].name);
        if (!delete_tree(sub, depth + 1)) ok = false;
    }
    free(v);
    return ok && plat_rmdir(path);
}

static void untick_path(const char *path)
{
    for (int i = 0; i < s_nsel;) {
        if (path_covers(path, s_sel[i])) {
            for (int k = i; k + 1 < s_nsel; k++) memcpy(s_sel[k], s_sel[k + 1], PATH_CAP);
            s_nsel--;
        } else {
            i++;
        }
    }
}

/* After the confirmation: the deleting runs in app_fileops.c (a dialog with a progress bar); it reports back below. */
void do_delete(void)
{
    if (s_ndel > 0 && delete_start(s_del, s_ndel)) go(SC_DELETE);
    else { s_ndel = 0; go(SC_FILES); }
}

void files_delete_finished(int ok, int bad, const char *first_bad, bool cancelled)
{
    for (int i = 0; i < s_ndel; i++)
        if (!plat_path_info(s_del[i], NULL, NULL)) untick_path(s_del[i]);           /* what is gone cannot stay ticked */
    s_ndel = 0;
    files_ensure();
    files_reload();
    if (A.sc == SC_DELETE) go(SC_FILES);
    if (cancelled) toast("Stopped - %d item%s deleted, the rest stays", ok, ok == 1 ? "" : "s");
    else if (bad) toast("Deleted %d, but %.50s could not be deleted", ok, first_bad);
    else toast("Deleted %d item%s", ok, ok == 1 ? "" : "s");
}

void files_focus_name(const char *name)
{
    files_ensure();
    files_reload();
    for (int i = 0; i < s_fb.nent; i++)
        if (!strcmp(s_fb.ent[i].name, name)) { s_fb.cur.sel = i; break; }
    ui_cursor_clamp(&s_fb.cur, s_fb.nent, BROWSER_ROWS);
}

static bool valid_file_name(const char *n)
{
    if (!n[0] || !strcmp(n, ".") || !strcmp(n, "..")) return false;
    for (const char *c = n; *c; c++)
        if ((unsigned char)*c < 32 || strchr("/\\:*?\"<>|", *c)) return false;
    return true;
}

/* ---- move: mark what to move, open the destination folder, "Move here" ---- */

/* Same folder, ignoring a trailing slash ("ux0:data/" and "ux0:data"). */
static bool same_dir(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    while (la > 1 && a[la - 1] == '/') la--;
    while (lb > 1 && b[lb - 1] == '/') lb--;
    return la == lb && strncmp(a, b, la) == 0;
}

/* A name for the copy that does not exist yet: "ux0:data/a.bin" -> "ux0:data/a (1).bin" ("name (1)" for a folder). */
bool unique_path(char *path, size_t cap, bool is_dir)
{
    if (!plat_path_info(path, NULL, NULL)) return true;
    char stem[PATH_CAP], ext[64] = "";
    ls_strlcpy(stem, path, sizeof stem);
    char *slash = strrchr(stem, '/');
    char *dot = strrchr(slash ? slash : stem, '.');
    if (!is_dir && dot && dot != (slash ? slash + 1 : stem) && strlen(dot) < sizeof ext) {
        ls_strlcpy(ext, dot, sizeof ext);
        *dot = 0;
    }
    for (int n = 1; n < 100; n++) {
        if (snprintf(path, cap, "%s (%d)%s", stem, n, ext) >= (int)cap) return false;
        if (!plat_path_info(path, NULL, NULL)) return true;
    }
    return false;
}

/* "Move" / "Copy": remembers the ticked items (or the highlighted one) until a destination folder is chosen. */
static void move_mark(bool copy)
{
    fbrowser *b = &s_fb;
    char list[MAX_SEL][PATH_CAP];
    int n = 0;
    if (s_nsel > 0) {
        for (int i = 0; i < s_nsel; i++) ls_strlcpy(list[n++], s_sel[i], PATH_CAP);
    } else if (b->nent > 0 && b->cur.sel < b->nent) {
        browser_entry_path(b, b->cur.sel, list[0], PATH_CAP);
        n = 1;
    }
    go(SC_FILES);
    if (n == 0) { toast(copy ? "Nothing to copy" : "Nothing to move"); return; }
    for (int i = 0; i < n; i++) {
        if (!copy && path_covers(list[i], plat_data_dir())) {            /* never move the folder with our own settings and certificate */
            toast("%.60s contains VitaSend's own data - not moved", base_name(list[i]));
            return;
        }
    }
    for (int i = 0; i < n; i++) ls_strlcpy(s_mv[i], list[i], PATH_CAP);
    s_nmv = n;
    s_mv_copy = copy;
    s_nsel = 0;                                                        /* the ticks were used up by this */
    const char *verb = copy ? "Copying" : "Moving";
    if (n == 1) toast("%s %.40s - open the destination folder, then press TRIANGLE", verb, base_name(list[0]));
    else toast("%s %d items - open the destination, then press TRIANGLE", verb, n);
}

/* "Move here" / "Copy here": puts the marked items into the folder that is open. A move inside one card is a rename (instant);
 * everything else is copied by app_move.c (a move removes the originals afterwards). Nothing is ever overwritten: a move skips
 * an existing name, a copy takes a new one ("a (1).bin"). */
static void move_here(void)
{
    fbrowser *b = &s_fb;
    if (s_nmv == 0) return;
    const bool copy_mode = s_mv_copy;
    if (!b->path[0]) { toast("Open a storage device or a folder first"); return; }
    int moved = 0, same = 0, exists = 0, inside = 0, failed = 0, copy = 0;
    char first_exists[96] = "";
    move_reset();
    for (int i = 0; i < s_nmv; i++) {
        char parent[PATH_CAP], dst[PATH_CAP];
        bool is_dir = false;
        ls_strlcpy(parent, s_mv[i], sizeof parent);
        path_parent(parent);
        path_join(dst, sizeof dst, b->path, base_name(s_mv[i]));
        if (path_covers(s_mv[i], b->path)) { inside++; continue; }       /* a folder into itself */
        if (copy_mode) {
            plat_path_info(s_mv[i], &is_dir, NULL);
            if (!unique_path(dst, sizeof dst, is_dir)) { failed++; continue; }     /* an existing name: the copy gets "(1)" */
            if (move_add(s_mv[i], dst)) copy++;
            else failed++;
            continue;
        }
        if (same_dir(parent, b->path)) { same++; continue; }
        if (plat_path_info(dst, NULL, NULL)) {
            if (!first_exists[0]) ls_strlcpy(first_exists, base_name(dst), sizeof first_exists);
            exists++;
            continue;
        }
        if (plat_rename(s_mv[i], dst)) { moved++; untick_path(s_mv[i]); continue; }
        if (move_add(s_mv[i], dst)) copy++;                              /* another card: copy, then remove the original */
        else failed++;
    }
    char dest_dir[PATH_CAP];
    ls_strlcpy(dest_dir, b->path, sizeof dest_dir);
    s_nmv = 0;
    ls_log("%s here %s: %d renamed, %d to copy, %d already there, %d same folder, %d into itself, %d failed",
           copy_mode ? "copy" : "move", dest_dir, moved, copy, exists, same, inside, failed);
    if (copy > 0) {
        if (move_start(dest_dir, copy_mode)) {
            s_pre_moved = moved;
            s_pre_skipped = exists + same + inside + failed;
            files_reload();
            go(SC_MOVE);                                                 /* the dialog; app_move.c reports when it is done */
        } else {
            files_reload();                                              /* (move_start said why: e.g. not enough free space) */
        }
        return;
    }
    files_reload();
    char msg[200];
    int len = snprintf(msg, sizeof msg, "Moved %d item%s", moved, moved == 1 ? "" : "s");
    if (exists > 0) len += snprintf(msg + len, sizeof msg - (size_t)len, " - %.40s exists already%s", first_exists, exists > 1 ? " (and more)" : "");
    if (same > 0) len += snprintf(msg + len, sizeof msg - (size_t)len, " - already in this folder");
    if (inside > 0) len += snprintf(msg + len, sizeof msg - (size_t)len, " - a folder cannot go into itself");
    if (failed > 0) snprintf(msg + len, sizeof msg - (size_t)len, " - %d failed", failed);
    if (copy_mode) {                                                     /* nothing was copied: only the reasons are left */
        if (inside > 0) toast("A folder cannot be copied into itself");
        else toast("Nothing was copied");
    } else {
        toast("%s", msg);
    }
}

/* app_move.c finished (or was cancelled): say what happened and show the folder as it is now. */
void files_move_finished(int ok, int bad, int left, bool cancelled, bool copy)
{
    files_ensure();
    files_reload();
    if (A.sc == SC_MOVE) go(SC_FILES);
    ok += s_pre_moved;
    char skipped[60] = "";
    if (s_pre_skipped > 0) snprintf(skipped, sizeof skipped, " - %d left alone (name exists / same folder)", s_pre_skipped);
    s_pre_moved = s_pre_skipped = 0;
    const char *done = copy ? "Copied" : "Moved";
    if (cancelled) toast(copy ? "Copy cancelled - %d item%s copied, the rest was not copied" : "Move cancelled - %d item%s moved, the rest stays where it was", ok, ok == 1 ? "" : "s");
    else if (bad > 0) toast("%s %d, %d could not be copied (the originals are untouched)%s", done, ok, bad, skipped);
    else if (left > 0) toast("Moved %d, but %d original%s could not be removed%s", ok, left, left == 1 ? "" : "s", skipped);
    else toast("%s %d item%s%s", done, ok, ok == 1 ? "" : "s", skipped);
}

static void menu_run(int id)
{
    fbrowser *b = &s_fb;
    bool have = b->nent > 0 && b->cur.sel < b->nent;
    char full[PATH_CAP] = "";
    if (have) browser_entry_path(b, b->cur.sel, full, sizeof full);
    switch (id) {
    case MI_TICKALL:
        for (int i = 0; i < b->nent; i++) {
            char p[PATH_CAP];
            browser_entry_path(b, i, p, sizeof p);
            if (!is_selected(p) && s_nsel < MAX_SEL) toggle_selected(p);
        }
        if (s_nsel >= MAX_SEL) toast("At most %d items can be ticked at once", MAX_SEL);
        go(SC_FILES);
        break;
    case MI_UNTICK:  s_nsel = 0; go(SC_FILES); break;
    case MI_RENAME:
        ls_strlcpy(s_op_path, full, sizeof s_op_path);
        open_osk(OP_RENAME, "Rename", b->ent[b->cur.sel].name, 120, false, SC_FILES);
        break;
    case MI_DELETE:  delete_ask(); break;
    case MI_UNZIP:   unzip_ui_open(full, SC_FILES); break;
    case MI_MOVE:    move_mark(false); break;
    case MI_COPY:    move_mark(true); break;
    case MI_MOVEHERE: go(SC_FILES); move_here(); break;
    case MI_MOVECANCEL: s_nmv = 0; toast(s_mv_copy ? "Copy cancelled" : "Move cancelled"); go(SC_FILES); break;
    case MI_NEWDIR:  open_osk(OP_NEWDIR, "Name of the new folder", "New folder", 120, false, SC_FILES); break;
    }
}

void menu_input(uint32_t p)
{
    if (p & UI_BTN_UP)   ui_cursor_move(&s_mcur, -1, s_nmenu, MENU_ROWS);
    if (p & UI_BTN_DOWN) ui_cursor_move(&s_mcur, 1, s_nmenu, MENU_ROWS);
    if (p & UI_BTN_CIRCLE) { go(SC_FILES); return; }
    if ((p & UI_BTN_CROSS) && s_mcur.sel < s_nmenu) menu_run(s_menu[s_mcur.sel].id);
}

/* The keyboard finished a rename / new folder. Returns false to stay on the keyboard (name not usable). */
bool files_name_entered(bool rename, const char *name)
{
    fbrowser *b = &s_fb;
    if (!valid_file_name(name)) {
        toast("That name cannot be used (no / \\ : * ? \" < > |)");
        return false;
    }
    char target[PATH_CAP];
    if (rename) {
        char dir[PATH_CAP];
        ls_strlcpy(dir, s_op_path, sizeof dir);
        path_parent(dir);
        path_join(target, sizeof target, dir, name);
        if (!strcmp(target, s_op_path)) return true;                      /* unchanged */
        if (plat_path_info(target, NULL, NULL)) { toast("\"%.60s\" exists already", name); return false; }
        if (!plat_rename(s_op_path, target)) { toast("Could not rename it"); return true; }
        for (int i = 0; i < s_nsel; i++)                                  /* a ticked item stays ticked */
            if (!strcmp(s_sel[i], s_op_path)) ls_strlcpy(s_sel[i], target, PATH_CAP);
        toast("Renamed to %.60s", name);
    } else {
        path_join(target, sizeof target, b->path, name);
        if (plat_path_info(target, NULL, NULL)) { toast("\"%.60s\" exists already", name); return false; }
        if (!plat_mkdirs(target)) { toast("Could not create the folder"); return true; }
        toast("Created %.60s", name);
    }
    char keep[256];
    ls_strlcpy(keep, name, sizeof keep);
    browser_load(b);
    files_select_name(keep);
    return true;
}

