/*
 * app_move.c - copying files and folders, and moving them to another memory card (the Files tab, "Copy" and "Move")
 *
 * Moving inside one memory card is just a rename and needs no code here (see app_files.c). Between two cards a rename is not
 * possible, so the data is copied first and the original is removed only after the copy of that item is complete: a failed or
 * cancelled move never loses anything. "Copy" is the same job without the last step: the originals always stay.
 *
 * The copy runs in small steps from the main loop (move_tick, about 30 ms of work per call) so that the screen keeps updating
 * and the O button can cancel it. A folder is walked with an explicit stack of directory listings (no recursion, no list of
 * all files in memory).
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"

#define MV_DEPTH 16                       /* deepest folder nesting that is copied */
#define MV_CHUNK (64 * 1024)

typedef struct {
    char         src[PATH_CAP], dst[PATH_CAP];
    plat_dirent *ent;
    int          n, i;
} mv_frame;

static struct {
    bool           active, cancel;
    bool           keep;                         /* a copy: the originals stay (otherwise they are removed after the copy) */
    char           src[MAX_SEL][PATH_CAP];       /* the items to copy (top level) ... */
    char           dst[MAX_SEL][PATH_CAP];       /* ... and where each one goes */
    char           dir[PATH_CAP];                /* the destination folder, for the dialog */
    int            n, cur;                       /* number of items, the one being copied */
    int            ok, bad, left;                /* moved, failed, "copied but the original could not be removed" */
    uint64_t       total, done;                  /* bytes */
    mv_frame       st[MV_DEPTH];
    int            depth;
    FILE          *in, *out;
    bool           item_active, item_bad;
    unsigned char *buf;
} J;

bool move_busy(void) { return J.active; }

/* ---- what to move ---- */

void move_reset(void)
{
    if (J.active) return;
    J.n = 0;
}

bool move_add(const char *src, const char *dst)
{
    if (J.active || J.n >= MAX_SEL) return false;
    ls_strlcpy(J.src[J.n], src, PATH_CAP);
    ls_strlcpy(J.dst[J.n], dst, PATH_CAP);
    J.n++;
    return true;
}

/* Adds up the bytes of a file or folder. False if the tree is too deep or cannot be read. */
static bool count_tree(const char *path, int depth, uint64_t *bytes)
{
    bool is_dir = false;
    uint64_t size = 0;
    if (!plat_path_info(path, &is_dir, &size)) return false;
    if (!is_dir) { *bytes += size; return true; }
    if (depth >= MV_DEPTH) return false;
    plat_dirent *v = NULL;
    int n = plat_list_dir(path, &v);
    bool ok = n >= 0;
    for (int i = 0; ok && i < n; i++) {
        char sub[PATH_CAP];
        if (snprintf(sub, sizeof sub, "%s/%s", path, v[i].name) >= (int)sizeof sub) { ok = false; break; }
        ok = count_tree(sub, depth + 1, bytes);
    }
    free(v);
    return ok;
}

/* Starts copying what move_add() collected into `dstdir` (keep_originals: a plain copy, otherwise a move between cards).
 * False (with a message) if there is not enough room. */
bool move_start(const char *dstdir, bool keep_originals)
{
    if (J.active || J.n == 0) return false;
    J.total = J.done = 0;
    for (int i = 0; i < J.n; i++) {
        uint64_t b = 0;
        if (count_tree(J.src[i], 0, &b)) J.total += b;
    }
    uint64_t room = plat_free_space(dstdir);
    if (room != UINT64_MAX && room < J.total) {
        char need[24], have[24];
        fmt_size(J.total, need, sizeof need);
        fmt_size(room, have, sizeof have);
        toast("Not enough free space here: %s needed, %s free", need, have);
        J.n = 0;
        return false;
    }
    J.buf = malloc(MV_CHUNK);
    if (!J.buf) { toast("Not enough memory to copy"); J.n = 0; return false; }
    ls_strlcpy(J.dir, dstdir, sizeof J.dir);
    J.cur = J.ok = J.bad = J.left = 0;
    J.depth = 0;
    J.in = J.out = NULL;
    J.item_active = J.item_bad = J.cancel = false;
    J.keep = keep_originals;
    J.active = true;
    ls_log("%s: copying %d item(s), %llu bytes, to %s", keep_originals ? "copy" : "move", J.n, (unsigned long long)J.total, dstdir);
    return true;
}

/* ---- one item ---- */

static void close_files(void)
{
    if (J.in)  { fclose(J.in);  J.in = NULL; }
    if (J.out) { if (fclose(J.out) != 0) J.item_bad = true; J.out = NULL; }
}

static void free_frames(void)
{
    while (J.depth > 0) free(J.st[--J.depth].ent);
}

/* The current item is finished (well or badly): remove the original / the partial copy and go to the next item. */
static void end_item(bool created)
{
    close_files();
    free_frames();
    int i = J.cur;
    if (!J.item_bad) {
        J.ok++;
        if (!J.keep && !delete_tree(J.src[i], 0)) J.left++;
    } else {
        J.bad++;
        if (created) delete_tree(J.dst[i], 0);           /* the half-written copy; the original was never touched */
    }
    J.cur++;
    J.item_active = false;
}

static bool open_file(const char *src, const char *dst)
{
    J.in = fopen(src, "rb");
    if (!J.in) return false;
    J.out = fopen(dst, "wb");
    if (!J.out) { fclose(J.in); J.in = NULL; return false; }
    return true;
}

static bool push_frame(const char *src, const char *dst)
{
    if (J.depth >= MV_DEPTH) return false;
    mv_frame *f = &J.st[J.depth];
    f->ent = NULL;
    f->n = plat_list_dir(src, &f->ent);
    if (f->n < 0) return false;
    f->i = 0;
    ls_strlcpy(f->src, src, sizeof f->src);
    ls_strlcpy(f->dst, dst, sizeof f->dst);
    J.depth++;
    return true;
}

static void start_item(void)
{
    int i = J.cur;
    bool is_dir = false;
    if (!plat_path_info(J.src[i], &is_dir, NULL) || plat_path_info(J.dst[i], NULL, NULL)) {
        J.bad++;                                         /* vanished, or something with that name appeared meanwhile: touch nothing */
        J.cur++;
        return;
    }
    J.item_active = true;
    J.item_bad = false;
    J.depth = 0;
    if (is_dir) {
        if (!plat_mkdirs(J.dst[i]) || !push_frame(J.src[i], J.dst[i])) { J.item_bad = true; end_item(true); }
    } else if (!open_file(J.src[i], J.dst[i])) {
        J.item_bad = true;
        end_item(false);
    }
}

static void copy_chunk(void)
{
#ifdef LS_HOST
    static int slow = -1;
    if (slow < 0) { const char *e = getenv("LS_SLOW_MOVE_MS"); slow = e ? atoi(e) : 0; }
    if (slow > 0) plat_sleep_ms((unsigned)slow);        /* tests: make the copy slow enough to be cancelled */
#endif
    size_t r = fread(J.buf, 1, MV_CHUNK, J.in);
    if (r > 0) {
        if (fwrite(J.buf, 1, r, J.out) != r) J.item_bad = true;
        J.done += r;
    }
    if (ferror(J.in)) J.item_bad = true;
    if (r < MV_CHUNK || J.item_bad) {                    /* end of this file (or an error) */
        close_files();
        if (J.item_bad) end_item(true);
        else if (J.depth == 0) end_item(true);           /* the item was a single file */
    }
}

static void walk_step(void)
{
    mv_frame *f = &J.st[J.depth - 1];
    if (f->i >= f->n) {
        free(f->ent);
        f->ent = NULL;
        J.depth--;
        if (J.depth == 0) end_item(true);
        return;
    }
    const plat_dirent *e = &f->ent[f->i++];
    char s[PATH_CAP], d[PATH_CAP];
    if (snprintf(s, sizeof s, "%s/%s", f->src, e->name) >= (int)sizeof s ||
        snprintf(d, sizeof d, "%s/%s", f->dst, e->name) >= (int)sizeof d) {
        J.item_bad = true;
    } else if (e->is_dir) {
        if (!plat_mkdirs(d) || !push_frame(s, d)) J.item_bad = true;
    } else if (!open_file(s, d)) {
        J.item_bad = true;
    }
    if (J.item_bad) end_item(true);
}

static void finish(bool cancelled)
{
    if (J.item_active) {                                 /* cancelled in the middle of an item: drop its partial copy */
        J.item_bad = true;
        end_item(true);
        J.bad--;                                         /* (not a failure: the user asked for it) */
    }
    free(J.buf);
    J.buf = NULL;
    J.active = false;
    int ok = J.ok, bad = J.bad, left = J.left;
    J.n = 0;
    ls_log("%s: %d done, %d failed, %d originals left%s", J.keep ? "copy" : "move", ok, bad, left, cancelled ? " (cancelled)" : "");
    files_move_finished(ok, bad, left, cancelled, J.keep);
}

/* Called from the main loop: copies for about 30 ms. */
void move_tick(void)
{
    if (!J.active) return;
    uint64_t t0 = plat_time_ms();
    do {
        if (J.cancel) { finish(true); return; }
        if (!J.item_active) {
            if (J.cur >= J.n) { finish(false); return; }
            start_item();
        } else if (J.in) {
            copy_chunk();
        } else if (J.depth > 0) {
            walk_step();
        } else {
            end_item(true);
        }
    } while (plat_time_ms() - t0 < 30);
}

/* ---- the dialog ---- */

void move_draw(const xfer_status *x)
{
    int dx, dy;
    ui_dialog_begin(SC_MOVE | (A.tab << 8), 640, 270, under_tab, (void *)x, &dx, &dy);
    int lx = dx + 32, w = 640 - 64;
    gfx_circle((float)(lx + 28), (float)(dy + 52), 28, C_ACCENT_BG);
    gfx_icon(IC_FOLDER, (float)(lx + 28), (float)(dy + 52), 30, C_ACCENT_HI);
    gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, J.keep ? "Copying" : "Moving");
    gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, J.dir);

    char line[200];
    int shown = J.cur < J.n ? J.cur + 1 : J.n;
    snprintf(line, sizeof line, "%d / %d  \xC2\xB7  %s", shown, J.n, J.cur < J.n ? base_name(J.src[J.cur]) : "");
    gfx_text_fit(lx, dy + 120, w, 17, GW_MED, C_TEXT, GA_LEFT, line);
    gfx_text_fit(lx, dy + 146, w, 13, GW_REG, C_DIM, GA_LEFT,
                 J.keep ? "The originals stay where they are." : "Copied to the other card first; the original is removed afterwards.");

    uint64_t total = J.total ? J.total : 1, done = J.done > total ? total : J.done;
    ui_progress(lx, dy + 168, w - 70, 12, done, total, C_ACCENT);
    char pcs[16];
    snprintf(pcs, sizeof pcs, "%d %%", (int)(done * 100 / total));
    gfx_text(lx + w, dy + 174, 17, GW_MED, C_TEXT, GA_RIGHT, pcs);

    ui_action(lx, dy + 220, PB_CIRCLE, "Cancel");
    ui_hints("O Cancel");
}

void move_input(uint32_t p)
{
    if (p & UI_BTN_CIRCLE) J.cancel = true;
}
