/*
 * app_files.c - the file browser (choose what to send) and the Files tab with its options menu
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"

/* ---- state of the file browser and the Files tab ---- */
static browser_mode s_bmode;
static fbrowser     s_sb;                    /* the "choose files to send" / "choose folder" page */
static fbrowser     s_fb;                    /* the Files tab (keeps its place while you use other tabs) */
static bool         s_fb_ready;
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
    if (b->path[0] && s_nsel > 0 && fr[0]) snprintf(head, sizeof head, "%d selected \xC2\xB7 %s free", s_nsel, fr);
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
    }
    if (b->nent > BROWSER_ROWS) {
        char cnt[32];
        snprintf(cnt, sizeof cnt, "%d / %d", b->cur.sel + 1, b->nent);
        right -= gfx_text(right, 100, 14, GW_REG, C_DIM, GA_RIGHT, cnt) + 14;
    }
    gfx_text_fit(CONTENT_X + 50, 100, right - CONTENT_X - 60, 16, GW_MED, C_TEXT, GA_LEFT, b->path[0] ? b->path : "Storage devices");

    ui_cursor_clamp(&b->cur, b->nent, BROWSER_ROWS);
    if (b->nent == 0) gfx_text(CONTENT_X + 24, BROWSER_TOP + 20, 16, GW_REG, C_DIM, GA_LEFT, "(empty)");
    bool cur_vpk = false, cur_dir = false;
    for (int i = 0; i < BROWSER_ROWS && b->cur.top + i < b->nent; i++) {
        int idx = b->cur.top + i;
        int y = BROWSER_TOP + i * BROWSER_STEP, cy = y + 17;
        const plat_dirent *e = &b->ent[idx];
        bool vpk = !e->is_dir && vpk_has_extension(e->name);
        bool cur = idx == b->cur.sel;
        ui_hit_row(CONTENT_X, y - 2, CONTENT_W - 14, BROWSER_STEP, &b->cur, idx, vpk ? UI_BTN_SQUARE : UI_BTN_CROSS);   /* tap: open a folder / ask to install a package / tick a file */
        if (b->path[0]) ui_hit_call(CONTENT_X, y - 2, 56, BROWSER_STEP, files_tick_tap, NULL, idx);       /* the tick circle */
        if (cur) {
            cur_vpk = vpk;
            cur_dir = e->is_dir;
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
        int rw = (e->is_dir ? 0 : 100) + (vpk ? 70 : 0);
        gfx_text_fit(tx + 34, cy, CONTENT_W - (tx - CONTENT_X) - 34 - 40 - rw, 16, e->is_dir || vpk ? GW_MED : GW_REG, C_TEXT, GA_LEFT, label);
        int rx = CONTENT_R - 34;
        if (!e->is_dir) {
            char sz[24];
            fmt_size(e->size, sz, sizeof sz);
            rx -= gfx_text(rx, cy, 14, GW_REG, C_DIM, GA_RIGHT, sz) + 10;
        }
        if (vpk) ui_chip(rx, cy, "VPK", C_ACCENT_HI);
        gfx_focus(false);
    }
    ui_scrollbar(CONTENT_R - 6, BROWSER_TOP, BROWSER_ROWS * BROWSER_STEP - 4, b->cur.top, BROWSER_ROWS, b->nent);
    ui_hit_scroll(CONTENT_X, BROWSER_TOP, CONTENT_W, BROWSER_ROWS * BROWSER_STEP, &b->cur.top, &b->cur.sel, b->nent, BROWSER_ROWS, BROWSER_STEP);

    if (!b->path[0]) ui_hints("DUD Move|X Open|LR Tabs");
    else ui_hints(cur_vpk ? "DUD Move|X Tick|SQ Install|TRI Send|SEL Menu|O Up|LR Tabs"
                          : (cur_dir ? "DUD Move|X Open|SQ Tick|TRI Send|SEL Menu|O Up|LR Tabs" : "DUD Move|X Tick|TRI Send|SEL Menu|O Up|LR Tabs"));
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

    if ((p & UI_BTN_CROSS) && have) {                      /* X: open a folder, or tick (select) a file */
        if (is_dir) {
            ls_strlcpy(b->path, full, sizeof b->path);
            browser_load(b);
        } else if (!b->path[0]) {
            toast("Open the storage device first");
        } else {
            toggle_selected(full);
        }
    }
    if ((p & UI_BTN_SQUARE) && have) {                    /* SQUARE: install a .vpk package; on a folder: tick it */
        if (is_vpk) { install_open(full, SC_FILES); return; }
        if (b->path[0]) toggle_selected(full);
    }
    if (p & UI_BTN_TRIANGLE) { files_send(); return; }
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

enum { MI_INSTALL, MI_INSTALLQ, MI_TICK, MI_SEND, MI_TICKALL, MI_UNTICK, MI_RENAME, MI_DELETE, MI_NEWDIR };
typedef struct { int id; char label[140]; } menu_item;
#define MENU_MAX 9
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

static bool is_selected_cur(void)
{
    fbrowser *b = &s_fb;
    char full[PATH_CAP];
    if (b->nent <= 0 || b->cur.sel >= b->nent) return false;
    browser_entry_path(b, b->cur.sel, full, sizeof full);
    return is_selected(full);
}

static void menu_open(void)
{
    fbrowser *b = &s_fb;
    files_ensure();
    if (!b->path[0]) { toast("Open a storage device first"); return; }
    bool have = b->nent > 0 && b->cur.sel < b->nent;
    const char *name = have ? b->ent[b->cur.sel].name : "";
    s_nmenu = 0;
    if (have && !b->ent[b->cur.sel].is_dir && vpk_has_extension(name)) menu_add(MI_INSTALL, "Install %.60s", name);
    if (have) menu_add(MI_TICK, "%s %.70s", is_selected_cur() ? "Untick" : "Tick", name);
    int nticked_vpk = 0;
    for (int i = 0; i < s_nsel; i++) if (vpk_has_extension(s_sel[i])) nticked_vpk++;
    if (nticked_vpk >= 2) menu_add(MI_INSTALLQ, "Install the %d ticked .vpk packages", nticked_vpk);
    if (s_nsel > 0) menu_add(MI_SEND, "Send the %d ticked item%s", s_nsel, s_nsel == 1 ? "" : "s");
    else if (have) menu_add(MI_SEND, "Send %.70s", name);
    if (b->nent > 0) menu_add(MI_TICKALL, "Tick everything in this folder");
    if (s_nsel > 0) menu_add(MI_UNTICK, "Untick everything (%d)", s_nsel);
    if (have) menu_add(MI_RENAME, "Rename %.70s", name);
    if (s_nsel > 0) menu_add(MI_DELETE, "Delete the %d ticked item%s", s_nsel, s_nsel == 1 ? "" : "s");
    else if (have) menu_add(MI_DELETE, "Delete %.70s", name);
    menu_add(MI_NEWDIR, "New folder here");
    memset(&s_mcur, 0, sizeof s_mcur);
    go(SC_MENU);
}

void menu_draw(const xfer_status *x)
{
    int h = 112 + s_nmenu * 46;
    int dx, dy;
    ui_dialog_begin(SC_MENU | (A.tab << 8), 560, h, under_tab, (void *)x, &dx, &dy);
    gfx_text(dx + 32, dy + 42, 22, GW_BOLD, C_TEXT, GA_LEFT, "Options");
    gfx_text_fit(dx + 32, dy + 70, 500, 13, GW_REG, C_DIM, GA_LEFT, s_fb.path);
    ui_cursor_clamp(&s_mcur, s_nmenu, MENU_MAX);
    for (int i = 0; i < s_nmenu; i++) {
        int y = dy + 92 + i * 46;
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
static bool delete_tree(const char *path, int depth)
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

void do_delete(void)
{
    int ok = 0, bad = 0;
    char first_bad[96] = "";
    for (int i = 0; i < s_ndel; i++) {
        if (delete_tree(s_del[i], 0)) ok++;
        else { if (!first_bad[0]) ls_strlcpy(first_bad, base_name(s_del[i]), sizeof first_bad); bad++; }
        untick_path(s_del[i]);
    }
    ls_log("deleted %d item(s), %d could not be deleted", ok, bad);
    s_ndel = 0;
    if (bad) toast("Deleted %d, but %.50s could not be deleted", ok, first_bad);
    else toast("Deleted %d item%s", ok, ok == 1 ? "" : "s");
    files_reload();
}

static bool valid_file_name(const char *n)
{
    if (!n[0] || !strcmp(n, ".") || !strcmp(n, "..")) return false;
    for (const char *c = n; *c; c++)
        if ((unsigned char)*c < 32 || strchr("/\\:*?\"<>|", *c)) return false;
    return true;
}

static void menu_run(int id)
{
    fbrowser *b = &s_fb;
    bool have = b->nent > 0 && b->cur.sel < b->nent;
    char full[PATH_CAP] = "";
    if (have) browser_entry_path(b, b->cur.sel, full, sizeof full);
    switch (id) {
    case MI_INSTALL: go(SC_FILES); install_open(full, SC_FILES); break;
    case MI_INSTALLQ: {
        const char *pp[MAX_SEL];
        int np = 0;
        for (int i = 0; i < s_nsel; i++) if (vpk_has_extension(s_sel[i])) pp[np++] = s_sel[i];
        go(SC_FILES);
        iq_open(pp, np, SC_FILES, false);
        break;
    }
    case MI_SEND:    go(SC_FILES); files_send(); break;
    case MI_TICK:    toggle_selected(full); go(SC_FILES); break;
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
    case MI_NEWDIR:  open_osk(OP_NEWDIR, "Name of the new folder", "New folder", 120, false, SC_FILES); break;
    }
}

void menu_input(uint32_t p)
{
    if (p & UI_BTN_UP)   ui_cursor_move(&s_mcur, -1, s_nmenu, MENU_MAX);
    if (p & UI_BTN_DOWN) ui_cursor_move(&s_mcur, 1, s_nmenu, MENU_MAX);
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

