/*
 * app_dialogs.c - on-screen keyboard, message viewer, incoming request, transfer progress, confirmations
 *
 * Author: trashman (VitaSend)
 */
#include "app_int.h"

/* ---- state of the dialogs ---- */
static osk_state    s_osk;
static osk_purpose  s_osk_purpose;
static screen_id    s_osk_return;

static char         s_msg_title[96];
static char         s_msg_body[HIST_TEXT_MAX];
static int          s_msg_top;
static screen_id    s_msg_return;
static confirm_what s_confirm;
static screen_id    s_confirm_return;
static rx_request   s_req;

static void osk_finished(bool ok);

/* ---- what app.c and the pages need from them ---- */
bool request_pending(void) { return rx_pending(&s_req); }
screen_id osk_return_screen(void) { return s_osk_return; }
bool osk_is_pin_prompt(void) { return A.sc == SC_OSK && s_osk_purpose == OP_SEND_PIN; }
void osk_screen_draw(void) { osk_draw(&s_osk, false); }
void osk_screen_input(uint32_t p)
{
    int r = osk_handle(&s_osk, p);
    if (r != OSK_CONTINUE) osk_finished(r == OSK_OK);
}
void confirm_open(confirm_what what, screen_id ret)
{
    s_confirm = what;
    s_confirm_return = ret;
    go(SC_CONFIRM);
}


void open_osk(osk_purpose purpose, const char *title, const char *initial, int max_len, bool digits, screen_id ret)
{
    osk_open(&s_osk, title, initial, max_len, digits);
    s_osk_purpose = purpose;
    s_osk_return = ret;
    go(SC_OSK);
}

/* ============================================================== message viewer */

#define MSG_ROWS 9

void show_message(const char *title, const char *body, screen_id ret)
{
    ls_strlcpy(s_msg_title, title, sizeof s_msg_title);
    ls_strlcpy(s_msg_body, body, sizeof s_msg_body);
    s_msg_top = 0;
    s_msg_return = ret;
    go(SC_MESSAGE);
}


void message_draw(const xfer_status *x)
{
    int dx, dy;
    ui_dialog_begin(SC_MESSAGE | (A.tab << 8), 720, 380, under_tab, (void *)x, &dx, &dy);
    gfx_icon(IC_MESSAGE, (float)(dx + 44), (float)(dy + 44), 30, C_ACCENT_HI);
    gfx_text_fit(dx + 76, dy + 44, 600, 21, GW_BOLD, C_TEXT, GA_LEFT, s_msg_title);
    gfx_line((float)(dx + 28), (float)(dy + 76), (float)(dx + 692), (float)(dy + 76), 1, C_BORDER);
    gfx_line_t lines[64];
    int n = gfx_wrap(s_msg_body, 17, GW_REG, 664, lines, 64);
    if (s_msg_top > n - MSG_ROWS) s_msg_top = n - MSG_ROWS;
    if (s_msg_top < 0) s_msg_top = 0;
    for (int i = 0; i < MSG_ROWS && s_msg_top + i < n; i++) {
        char buf[200];
        int len = lines[s_msg_top + i].len;
        if (len >= (int)sizeof buf) len = (int)sizeof buf - 1;
        memcpy(buf, lines[s_msg_top + i].p, (size_t)len);
        buf[len] = 0;
        gfx_text(dx + 28, dy + 106 + i * 29, 17, GW_REG, C_TEXT, GA_LEFT, buf);
    }
    ui_scrollbar(dx + 704, dy + 92, 270, s_msg_top, MSG_ROWS, n);
    ui_hit_scroll(dx + 20, dy + 90, 680, 280, &s_msg_top, NULL, n, MSG_ROWS, 29);
    ui_hints(n > MSG_ROWS ? "DUD Scroll|X Close|O Close" : "X Close|O Close");
}

void message_input(uint32_t p)
{
    if (p & UI_BTN_UP)   s_msg_top--;
    if (p & UI_BTN_DOWN) s_msg_top++;
    if (p & (UI_BTN_CIRCLE | UI_BTN_CROSS)) go(s_msg_return);
}

/* ============================================================== incoming request */

void request_draw(const xfer_status *x)
{
    int dx, dy;
    ui_dialog_begin(SC_REQUEST | (A.tab << 8), 740, 400, under_tab, (void *)x, &dx, &dy);
    int lx = dx + 32;
    gfx_rrect(lx, dy + 24, 56, 56, 16, C_ACCENT_BG);
    ui_device_icon("desktop", s_req.model, (float)(lx + 28), (float)(dy + 52), 32, C_ACCENT_HI);
    gfx_text(lx + 76, dy + 30, 13, GW_REG, C_DIM, GA_LEFT, "Incoming request");
    gfx_text_fit(lx + 76, dy + 56, 560, 22, GW_BOLD, C_TEXT, GA_LEFT, s_req.alias);
    char who[100];
    if (s_req.model[0]) snprintf(who, sizeof who, "%s \xC2\xB7 %s", s_req.model, s_req.ip);
    else snprintf(who, sizeof who, "%s", s_req.ip);
    gfx_text_fit(lx + 76, dy + 80, 560, 14, GW_REG, C_DIM, GA_LEFT, who);

    char sz[24];
    fmt_size(s_req.total, sz, sizeof sz);
    if (s_req.is_text) {
        gfx_text(lx, dy + 124, 16, GW_REG, C_TEXT, GA_LEFT, "wants to send you a message:");
        gfx_rrect(lx, dy + 146, 676, 150, 12, GRGB(18, 23, 28));
        gfx_line_t lines[8];
        int n = gfx_wrap(s_req.text, 17, GW_REG, 644, lines, 5);
        for (int i = 0; i < n; i++) {
            char buf[200];
            int len = lines[i].len >= (int)sizeof buf ? (int)sizeof buf - 1 : lines[i].len;
            memcpy(buf, lines[i].p, (size_t)len);
            buf[len] = 0;
            gfx_text(lx + 16, dy + 172 + i * 27, 17, GW_REG, C_WARN, GA_LEFT, buf);
        }
    } else {
        char head[100];
        snprintf(head, sizeof head, "wants to send you %d file%s (%s):", s_req.nfiles, s_req.nfiles == 1 ? "" : "s", sz);
        gfx_text(lx, dy + 124, 16, GW_REG, C_TEXT, GA_LEFT, head);
        gfx_rrect(lx, dy + 146, 676, 130, 12, GRGB(18, 23, 28));
        for (int i = 0; i < 4 && i < s_req.nfiles; i++) {
            gfx_icon(IC_FILE, (float)(lx + 24), (float)(dy + 170 + i * 26), 18, C_DIM);
            gfx_text_fit(lx + 44, dy + 170 + i * 26, 610, 15, GW_REG, C_WARN, GA_LEFT, s_req.names[i]);
        }
        if (s_req.nfiles > 4) {
            char more[40];
            snprintf(more, sizeof more, "... and %d more", s_req.nfiles - 4);
            gfx_text(lx + 44, dy + 170 + 4 * 26 - 4, 13, GW_REG, C_DIM, GA_LEFT, more);
        }
        ls_settings st;
        settings_snapshot(&st);
        gfx_text(lx, dy + 298, 13, GW_REG, C_DIM, GA_LEFT, "They will be saved in:");
        if (s_req.nexist > 0) {
            char ex[120];
            snprintf(ex, sizeof ex, "%d already exist%s - %s", s_req.nexist, s_req.nexist == 1 ? "s" : "",
                     s_req.on_exists == EXISTS_SKIP ? "skipped" : s_req.on_exists == EXISTS_OVERWRITE ? "will be OVERWRITTEN" : "kept, new ones get (1)");
            gfx_text_fit(lx + 676, dy + 298, 400, 13, GW_MED, s_req.on_exists == EXISTS_OVERWRITE ? C_WARN : C_DIM, GA_RIGHT, ex);
        }
        gfx_text_fit(lx, dy + 320, 676, 15, GW_MED, C_TEXT, GA_LEFT, st.receive_dir);
    }
    int by = dy + 366;
    int w1 = ui_action_width(PB_CROSS, "Accept"), w2 = ui_action_width(PB_SQUARE, "Accept + make favorite"), w3 = ui_action_width(PB_CIRCLE, "Reject");
    int bx = dx + 370 - (w1 + w2 + w3 + 20) / 2 + 0;
    if (bx < lx) bx = lx;
    ui_action(bx, by, PB_CROSS, "Accept");
    ui_action(bx + w1 + 10, by, PB_SQUARE, "Accept + make favorite");
    ui_action(bx + w1 + w2 + 20, by, PB_CIRCLE, "Reject");
    ui_hints("X Accept|SQ Accept + favorite|O Reject");
}

void request_input(uint32_t p)
{
    if (p & (UI_BTN_CROSS | UI_BTN_SQUARE)) {
        if ((p & UI_BTN_SQUARE) && s_req.fp[0] && !settings_is_fav(s_req.fp)) settings_toggle_fav(s_req.fp, s_req.alias);
        rx_decide(true);
        go(A.ret);
    } else if (p & UI_BTN_CIRCLE) {
        rx_decide(false);
        go(A.ret);
    }
}

/* ============================================================== transfer progress */

void transfer_draw(const xfer_status *x)
{
    bool ended = x->state == XF_DONE || x->state == XF_FAILED || x->state == XF_CANCELLED;
    int dx, dy;
    ui_dialog_begin(SC_TRANSFER | (A.tab << 8), 680, 340, under_tab, (void *)x, &dx, &dy);
    int lx = dx + 32, w = 680 - 64;

    gcol mc = x->state == XF_DONE ? C_GOOD : (x->state == XF_FAILED ? C_BAD : (x->state == XF_CANCELLED ? C_WARN : C_TEXT));
    gcol tint = x->state == XF_DONE ? GRGBA(102, 187, 106, 40) : (x->state == XF_FAILED ? GRGBA(239, 83, 80, 40) : (x->state == XF_CANCELLED ? GRGBA(255, 193, 77, 40) : C_ACCENT_BG));
    gfx_circle((float)(lx + 28), (float)(dy + 52), 28, tint);
    int ic = x->state == XF_DONE ? IC_CHECK : (x->state == XF_FAILED ? IC_CLOSE : (x->state == XF_CANCELLED ? IC_CLOSE : (x->outgoing ? IC_ARROW_UP : IC_ARROW_DOWN)));
    gfx_icon(ic, (float)(lx + 28), (float)(dy + 52), 30, ended ? mc : C_ACCENT_HI);
    gfx_text(lx + 72, dy + 40, 22, GW_BOLD, C_TEXT, GA_LEFT, x->outgoing ? "Sending" : "Receiving");
    char who[120];
    snprintf(who, sizeof who, "%s %s", x->outgoing ? "To" : "From", x->peer);
    gfx_text_fit(lx + 72, dy + 68, w - 72, 15, GW_REG, C_DIM, GA_LEFT, who);

    gfx_text_fit(lx, dy + 112, w, 17, GW_MED, mc, GA_LEFT, x->message);

    if (x->files_total > 0) {
        int shown = x->files_done + ((x->state == XF_ACTIVE && x->files_done < x->files_total) ? 1 : 0);
        char fl[40];
        snprintf(fl, sizeof fl, "File %d of %d", shown, x->files_total);
        gfx_text(lx, dy + 150, 14, GW_REG, C_DIM, GA_LEFT, fl);
        if (x->current[0]) gfx_text_fit(lx + 110, dy + 150, w - 110, 14, GW_REG, C_TEXT, GA_LEFT, x->current);
    }
    int pct = xfer_percent(x);
    ui_progress(lx, dy + 184, w - 70, 12, x->bytes_done, x->bytes_total ? x->bytes_total : (x->state == XF_DONE ? 1 : 0),
                x->state == XF_FAILED ? C_BAD : C_ACCENT);
    if (x->state == XF_DONE && !x->bytes_total) ui_progress(lx, dy + 184, w - 70, 12, 1, 1, C_ACCENT);
    char pcs[16];
    snprintf(pcs, sizeof pcs, "%d %%", pct);
    gfx_text(lx + w, dy + 190, 17, GW_MED, C_TEXT, GA_RIGHT, pcs);

    char a[24], b[24], sp[24], line[120];
    fmt_size(x->bytes_done, a, sizeof a);
    fmt_size(x->bytes_total, b, sizeof b);
    snprintf(line, sizeof line, "%s of %s", a, b);
    gfx_text(lx, dy + 224, 15, GW_REG, C_TEXT, GA_LEFT, line);
    if (x->state == XF_ACTIVE && x->speed_bps > 0) {
        fmt_size(x->speed_bps, sp, sizeof sp);
        char eta[32];
        uint64_t left = x->bytes_total > x->bytes_done ? x->bytes_total - x->bytes_done : 0;
        fmt_duration(left / x->speed_bps, eta, sizeof eta);
        snprintf(line, sizeof line, "%s/s \xC2\xB7 about %s left", sp, eta);
        gfx_text(lx + w, dy + 224, 15, GW_REG, C_DIM, GA_RIGHT, line);
    }
    if (x->state == XF_WAITING && x->outgoing) gfx_text(lx, dy + 252, 15, GW_REG, C_DIM, GA_LEFT, "Waiting for the other device to accept...");

    char vp[RX_VPK_MAX_UI][RX_PATH_CAP];
    int nvpk = (x->state == XF_DONE && !x->outgoing) ? rx_received_vpks(vp, RX_VPK_MAX_UI) : 0;
    if (ended && nvpk > 0) {
        char lab[64];
        if (nvpk == 1) snprintf(lab, sizeof lab, "Install %.40s", base_name(vp[0]));
        else snprintf(lab, sizeof lab, "Install all %d packages", nvpk);
        int w1 = ui_action_width(PB_CROSS, "Close");
        ui_action(lx, dy + 298, PB_CROSS, "Close");
        ui_action(lx + w1 + 10, dy + 298, PB_TRIANGLE, lab);
        ui_hints("X Close|TRI Install .vpk");
    } else if (ended && x->outgoing && x->state != XF_DONE && send_retry_count() > 0) {
        int nr = send_retry_count();
        char lab[64];
        if (nr == 1) snprintf(lab, sizeof lab, "Retry (1 left)");
        else snprintf(lab, sizeof lab, "Retry (%d files left)", nr);
        int w1 = ui_action_width(PB_CROSS, "Close");
        ui_action(lx, dy + 298, PB_CROSS, "Close");
        ui_action(lx + w1 + 10, dy + 298, PB_TRIANGLE, lab);
        ui_hints("X Close|TRI Retry");
    } else if (ended) {
        ui_action(lx, dy + 298, PB_CROSS, "Close");
        ui_hints("X Close");
    } else {
        int w1 = ui_action_width(PB_CIRCLE, "Cancel transfer");
        ui_action(lx, dy + 298, PB_CIRCLE, "Cancel transfer");
        ui_action(lx + w1 + 10, dy + 298, PB_TRIANGLE, "Hide (keeps running)");
        ui_hints("O Cancel transfer|TRI Hide (keeps running)");
    }
}

void transfer_input(uint32_t p)
{
    xfer_status x;
    xfer_snapshot(&x);
    bool ended = x.state == XF_DONE || x.state == XF_FAILED || x.state == XF_CANCELLED || x.state == XF_IDLE;
    if (ended) {
        if ((p & UI_BTN_TRIANGLE) && x.state == XF_DONE && !x.outgoing) {
            char vp[RX_VPK_MAX_UI][RX_PATH_CAP];
            int n = rx_received_vpks(vp, RX_VPK_MAX_UI);
            if (n == 1) { install_open(vp[0], (screen_id)A.tab); return; }
            if (n > 1) {
                const char *pp[RX_VPK_MAX_UI];
                for (int i = 0; i < n; i++) pp[i] = vp[i];
                iq_open(pp, n, (screen_id)A.tab, false);
                return;
            }
        }
        if ((p & UI_BTN_TRIANGLE) && x.outgoing && x.state != XF_DONE && x.state != XF_IDLE && send_retry_count() > 0) {
            if (!send_retry()) toast("Cannot retry right now");
            return;
        }
        if (p & (UI_BTN_CROSS | UI_BTN_CIRCLE)) {
            if (A.tab == SC_FILES) files_reload();          /* new files may have arrived in the folder we are looking at */
            go(A.tab);
        }
        return;
    }
    if (p & UI_BTN_TRIANGLE) go(A.tab);
    if (p & UI_BTN_CIRCLE) { s_confirm = CF_CANCEL_XFER; s_confirm_return = SC_TRANSFER; go(SC_CONFIRM); }
}


/* ============================================================== confirm */

static void under_confirm(void *ctx)
{
    if (s_confirm_return == SC_TRANSFER) transfer_draw(ctx);
    else draw_tab(s_confirm_return, ctx);
}

void confirm_draw(const xfer_status *x)
{
    int dx, dy;
    if (s_confirm == CF_SETTING) { setting_confirm_draw(x); return; }
    ui_dialog_begin(SC_CONFIRM | (A.tab << 8) | (s_confirm_return << 12) | (s_confirm << 20), 560, 200, under_confirm, (void *)x, &dx, &dy);
    if (s_confirm == CF_DELETE) {
        char q[160];
        delete_question(q, sizeof q);
        gfx_text(dx + 32, dy + 46, 22, GW_BOLD, C_TEXT, GA_LEFT, "Delete?");
        gfx_text_fit(dx + 32, dy + 88, 500, 16, GW_REG, C_TEXT, GA_LEFT, q);
        gfx_text_fit(dx + 32, dy + 114, 500, 13, GW_REG, C_DIM, GA_LEFT, "Folders go with everything inside. This cannot be undone.");
    } else if (s_confirm == CF_CLEAR_HISTORY) {
        char q[120];
        int n = history_count();
        snprintf(q, sizeof q, "Remove all %d entr%s from the History list?", n, n == 1 ? "y" : "ies");
        gfx_text(dx + 32, dy + 46, 22, GW_BOLD, C_TEXT, GA_LEFT, "Clear history?");
        gfx_text_fit(dx + 32, dy + 88, 500, 16, GW_REG, C_TEXT, GA_LEFT, q);
        gfx_text_fit(dx + 32, dy + 114, 500, 13, GW_REG, C_DIM, GA_LEFT, "The files themselves stay. This cannot be undone.");
    } else {
        gfx_text(dx + 32, dy + 46, 22, GW_BOLD, C_TEXT, GA_LEFT, "Are you sure?");
        gfx_text_fit(dx + 32, dy + 92, 500, 16, GW_REG, C_TEXT, GA_LEFT,
                     s_confirm == CF_QUIT ? "Quit VitaSend? Other devices will no longer see this Vita."
                                          : "Cancel the running transfer?");
    }
    int w = ui_action_width(PB_CROSS, "Yes");
    ui_action(dx + 32, dy + 150, PB_CROSS, "Yes");
    ui_action(dx + 32 + w + 10, dy + 150, PB_CIRCLE, "No");
    ui_hints("X Yes|O No");
}

void confirm_input(uint32_t p)
{
    if (p & UI_BTN_CROSS) {
        if (s_confirm == CF_SETTING) { pick_apply(); go(SC_SETTINGS); }
        else if (s_confirm == CF_QUIT) { A.quit = true; }
        else if (s_confirm == CF_DELETE) { do_delete(); }
        else if (s_confirm == CF_CLEAR_HISTORY) { history_clear(); history_reset(); toast("History cleared"); go(s_confirm_return); }
        else { xfer_request_cancel(); go(SC_TRANSFER); }
    } else if (p & UI_BTN_CIRCLE) {
        go(s_confirm_return);
    }
}

/* ============================================================== on-screen keyboard results */

static void osk_finished(bool ok)
{
    screen_id ret = s_osk_return;
    switch (s_osk_purpose) {
    case OP_ALIAS:
        if (ok) {
            if (!s_osk.text[0]) { toast("The name cannot be empty"); return; }
            identity_set_alias(s_osk.text);
            discovery_request_announce();
            toast("Now called %s", s_osk.text);
        }
        break;
    case OP_PIN_SET:
        if (ok) {
            settings_set_pin(s_osk.text);
            toast(s_osk.text[0] ? "PIN set" : "PIN removed");
        }
        break;
    case OP_SEND_PIN:
        send_give_pin(ok && s_osk.text[0] ? s_osk.text : NULL);
        A.pin_answered = true;
        break;
    case OP_ADD_IP:
        if (ok) {
            unsigned a, b, c, d, port = LS_DEFAULT_PORT;
            char tail;
            int k = sscanf(s_osk.text, "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &port, &tail);
            if (k < 4 || k > 5 || a > 255 || b > 255 || c > 255 || d > 255 || port < 1 || port > 65535) {
                toast("That is not an IP address like 192.168.0.12");
                return;
            }
            char ip[16];
            snprintf(ip, sizeof ip, "%u.%u.%u.%u", a, b, c, d);
            if (discovery_add_manual(ip, (uint16_t)port)) toast("Contacting %s ...", ip);
            else toast("Still busy with the last address, try again in a moment");
        }
        break;
    case OP_RENAME:
    case OP_NEWDIR:
        if (ok && !files_name_entered(s_osk_purpose == OP_RENAME, s_osk.text)) return;
        break;
    case OP_MESSAGE:
        if (ok) {
            if (!s_osk.text[0]) { toast("Type a message first"); return; }
            send_set_text(s_osk.text);
            toast("Message ready - pick a device and press X");
        }
        break;
    }
    go(ret);
}


