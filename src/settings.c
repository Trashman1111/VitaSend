/*
 * settings.c - The settings (config.json: name, port, receive folder, Quick Save, PIN, favorites, theme ...): load, save, thread-safe snapshots.
 *
 * Author: trashman (VitaSend)
 */
#include "settings.h"
#include "json.h"
#include "plat.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ls_settings s_set;
static plat_mutex *s_lock;
static char        s_path[300];

static void lock(void)
{
    if (!s_lock) s_lock = plat_mutex_create();     /* first call happens on the main thread, before any worker */
    plat_mutex_lock(s_lock);
}
static void unlock(void) { plat_mutex_unlock(s_lock); }

static void defaults(const char *dir)
{
    memset(&s_set, 0, sizeof s_set);
    s_set.port = LS_DEFAULT_PORT;
    ls_path_join(s_set.receive_dir, sizeof s_set.receive_dir, dir, "received");
    s_set.auto_accept = AUTO_ASK;
    s_set.sound = true;
    s_set.show_battery = true;
    s_set.screensaver = 30;
}

void settings_load(const char *dir)
{
    lock();
    defaults(dir);
    ls_path_join(s_path, sizeof s_path, dir, "config.json");

    uint8_t *data = NULL;
    size_t len = 0;
    if (ls_file_read(s_path, &data, &len)) {
        char err[80];
        json_val *j = json_parse((const char *)data, len, err, sizeof err);
        if (j && j->type == J_OBJ) {
            const char *a = json_str(json_get(j, "alias"), "");
            if (*a && strlen(a) < sizeof s_set.alias) ls_strlcpy(s_set.alias, a, sizeof s_set.alias);
            double p = json_num(json_get(j, "port"), LS_DEFAULT_PORT);
            if (p >= 1024 && p <= 65535) s_set.port = (uint16_t)p;
            const char *rd = json_str(json_get(j, "receiveDir"), "");
            if (*rd && strlen(rd) < sizeof s_set.receive_dir) ls_strlcpy(s_set.receive_dir, rd, sizeof s_set.receive_dir);
            /* up to v0.7.0x3 the app folder was ux0:data/LocalSendVita; it was renamed to VitaSend (see plat_data_dir) */
            static const char old_dir[] = "ux0:data/LocalSendVita";
            if (!strncmp(s_set.receive_dir, old_dir, sizeof old_dir - 1) && strncmp(dir, old_dir, sizeof old_dir - 1) != 0) {
                char tail[sizeof s_set.receive_dir];
                ls_strlcpy(tail, s_set.receive_dir + sizeof old_dir - 1, sizeof tail);
                ls_strlcpy(s_set.receive_dir, "ux0:data/VitaSend", sizeof s_set.receive_dir);
                size_t at = strlen(s_set.receive_dir);
                ls_strlcpy(s_set.receive_dir + at, tail, sizeof s_set.receive_dir - at);
            }
            double aa = json_num(json_get(j, "autoAccept"), 0);
            if (aa >= 0 && aa <= 2) s_set.auto_accept = (int)aa;
            double oe = json_num(json_get(j, "onExists"), 0);
            if (oe >= 0 && oe <= 2) s_set.on_exists = (int)oe;
            double ai = json_num(json_get(j, "autoInstall"), 0);
            if (ai >= 0 && ai <= 2) s_set.auto_install = (int)ai;
            s_set.sound = json_bool(json_get(j, "sound"), true);
            s_set.show_battery = json_bool(json_get(j, "showBattery"), true);
            s_set.show_clock = json_bool(json_get(j, "showClock"), false);
            double ss = json_num(json_get(j, "screensaver"), 30);
            if (ss >= 0 && ss <= 3600) s_set.screensaver = (int)ss;
            s_set.hide_ids = json_bool(json_get(j, "hideIds"), false);
            double th = json_num(json_get(j, "theme"), 0);
            s_set.theme = (th >= 1 && th < 2) ? 1 : 0;
            const char *pin = json_str(json_get(j, "pin"), "");
            bool digits = strlen(pin) <= SET_PIN_MAX;
            for (const char *c = pin; *c && digits; c++) digits = isdigit((unsigned char)*c) != 0;
            if (digits) ls_strlcpy(s_set.pin, pin, sizeof s_set.pin);
            const json_val *fav = json_get(j, "favorites");
            if (fav && fav->type == J_ARR) {
                for (size_t i = 0; i < fav->n && s_set.nfavs < SET_MAX_FAVS; i++) {
                    const char *fp = json_str(json_get(fav->items[i], "fingerprint"), "");
                    if (!*fp || strlen(fp) >= sizeof s_set.favs[0].fp) continue;
                    ls_strlcpy(s_set.favs[s_set.nfavs].fp, fp, sizeof s_set.favs[0].fp);
                    ls_strlcpy(s_set.favs[s_set.nfavs].alias, json_str(json_get(fav->items[i], "alias"), ""),
                               sizeof s_set.favs[0].alias);
                    s_set.nfavs++;
                }
            }
        } else {
            ls_log("config.json is not valid JSON (%s) - using defaults", err);
        }
        json_free(j);
        free(data);
    }
    unlock();
}

static void save_locked(void)
{
    if (!s_path[0]) return;
    jw w;
    jw_init(&w);
    jw_obj(&w);
    jw_kstr(&w, "alias", s_set.alias);
    jw_kint(&w, "port", s_set.port);
    jw_kstr(&w, "receiveDir", s_set.receive_dir);
    jw_kint(&w, "autoAccept", s_set.auto_accept);
    jw_kint(&w, "onExists", s_set.on_exists);
    jw_kint(&w, "autoInstall", s_set.auto_install);
    jw_kbool(&w, "sound", s_set.sound);
    jw_kbool(&w, "showBattery", s_set.show_battery);
    jw_kbool(&w, "showClock", s_set.show_clock);
    jw_kint(&w, "screensaver", s_set.screensaver);
    jw_kbool(&w, "hideIds", s_set.hide_ids);
    jw_kint(&w, "theme", s_set.theme);
    jw_kstr(&w, "pin", s_set.pin);
    jw_key(&w, "favorites");
    jw_arr(&w);
    for (int i = 0; i < s_set.nfavs; i++) {
        jw_obj(&w);
        jw_kstr(&w, "fingerprint", s_set.favs[i].fp);
        jw_kstr(&w, "alias", s_set.favs[i].alias);
        jw_end_obj(&w);
    }
    jw_end_arr(&w);
    jw_end_obj(&w);
    if (!w.oom) {
        /* write to a temp file first so a power cut cannot leave a half-written config */
        char tmp[320];
        snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
        if (ls_file_write(tmp, w.buf, w.len)) {
            plat_remove(s_path);
            if (!plat_rename(tmp, s_path)) ls_log("warning: could not replace %s", s_path);
        } else {
            ls_log("warning: could not write %s", tmp);
        }
    }
    jw_free(&w);
}

void settings_save(void)
{
    lock();
    save_locked();
    unlock();
}

void settings_snapshot(ls_settings *out)
{
    lock();
    *out = s_set;
    unlock();
}

void settings_set_alias(const char *alias)
{
    lock();
    ls_strlcpy(s_set.alias, alias, sizeof s_set.alias);
    save_locked();
    unlock();
}

void settings_set_port(uint16_t port)
{
    lock();
    s_set.port = port;
    save_locked();
    unlock();
}

void settings_set_receive_dir(const char *dir)
{
    lock();
    ls_strlcpy(s_set.receive_dir, dir, sizeof s_set.receive_dir);
    save_locked();
    unlock();
}

void settings_set_auto_accept(int mode)
{
    lock();
    s_set.auto_accept = (mode >= 0 && mode <= 2) ? mode : 0;
    save_locked();
    unlock();
}

void settings_set_on_exists(int mode)
{
    lock();
    s_set.on_exists = (mode >= EXISTS_KEEP_BOTH && mode <= EXISTS_OVERWRITE) ? mode : EXISTS_KEEP_BOTH;
    save_locked();
    unlock();
}

void settings_set_auto_install(int mode)
{
    lock();
    s_set.auto_install = (mode >= INSTALL_ASK && mode <= INSTALL_EVERYONE) ? mode : INSTALL_ASK;
    save_locked();
    unlock();
}

void settings_set_sound(bool on)
{
    lock();
    s_set.sound = on;
    save_locked();
    unlock();
}

void settings_set_show_battery(bool on)
{
    lock();
    s_set.show_battery = on;
    save_locked();
    unlock();
}

void settings_set_show_clock(bool on)
{
    lock();
    s_set.show_clock = on;
    save_locked();
    unlock();
}

void settings_set_screensaver(int seconds)
{
    lock();
    s_set.screensaver = (seconds >= 0 && seconds <= 3600) ? seconds : 30;
    save_locked();
    unlock();
}

void settings_set_hide_ids(bool hide)
{
    lock();
    s_set.hide_ids = hide;
    save_locked();
    unlock();
}

void settings_set_theme(int theme)
{
    lock();
    s_set.theme = theme == 1 ? 1 : 0;
    save_locked();
    unlock();
}

void settings_set_pin(const char *pin)
{
    lock();
    s_set.pin[0] = 0;
    size_t n = 0;
    for (const char *c = pin; *c && n < SET_PIN_MAX; c++)
        if (isdigit((unsigned char)*c)) s_set.pin[n++] = *c;
    s_set.pin[n] = 0;
    save_locked();
    unlock();
}

bool settings_is_fav(const char *fp)
{
    bool r = false;
    lock();
    for (int i = 0; i < s_set.nfavs; i++)
        if (!strcmp(s_set.favs[i].fp, fp)) { r = true; break; }
    unlock();
    return r;
}

bool settings_toggle_fav(const char *fp, const char *alias)
{
    bool now = false;
    lock();
    int idx = -1;
    for (int i = 0; i < s_set.nfavs; i++)
        if (!strcmp(s_set.favs[i].fp, fp)) { idx = i; break; }
    if (idx >= 0) {
        for (int i = idx; i + 1 < s_set.nfavs; i++) s_set.favs[i] = s_set.favs[i + 1];
        s_set.nfavs--;
    } else if (s_set.nfavs < SET_MAX_FAVS && strlen(fp) < sizeof s_set.favs[0].fp) {
        ls_strlcpy(s_set.favs[s_set.nfavs].fp, fp, sizeof s_set.favs[0].fp);
        ls_strlcpy(s_set.favs[s_set.nfavs].alias, alias, sizeof s_set.favs[0].alias);
        s_set.nfavs++;
        now = true;
    }
    save_locked();
    unlock();
    return now;
}
