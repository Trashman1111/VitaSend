/*
 * settings.h - everything the user can change, stored in <data dir>/config.json.
 *
 * All functions are thread safe (the HTTP threads read the settings, the UI thread changes them).
 *
 * Author: trashman (VitaSend)
 */
#ifndef LS_SETTINGS_H
#define LS_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SET_MAX_FAVS   16
#define SET_PIN_MAX    8

enum { AUTO_ASK = 0, AUTO_FAVORITES = 1, AUTO_EVERYONE = 2 };
/* what to do with a received file whose name is already taken in the receive folder */
enum { EXISTS_KEEP_BOTH = 0, EXISTS_SKIP = 1, EXISTS_OVERWRITE = 2 };
/* install a received .vpk without asking first (off by default - it runs code from the sender) */
enum { INSTALL_ASK = 0, INSTALL_FAVORITES = 1, INSTALL_EVERYONE = 2 };

typedef struct {
    char     alias[64];
    uint16_t port;
    char     receive_dir[256];
    int      auto_accept;                /* AUTO_* */
    int      on_exists;                  /* EXISTS_* */
    int      auto_install;               /* INSTALL_* */
    bool     sound;                      /* play a short chime when a transfer / installation has finished */
    bool     show_battery;               /* battery percentage + symbol, top right (default on) */
    bool     show_clock;                 /* wall-clock time next to it (default off)              */
    int      screensaver;                /* seconds without input before the screensaver starts; 0 = never */
    int      theme;                      /* THEME_DARK / THEME_OLED (see ui.h) */
    bool     hide_ids;                   /* hide this device's IP address and fingerprint on screen */
    bool     check_updates;              /* look for a new version on GitHub when the app starts (default on) */
    int      update_asked_day;           /* YYYYMMDD of the last day the "new version" question was shown (0 = never) */
    char     pin[SET_PIN_MAX + 1];       /* "" = no PIN required to send to us */
    char     wifi_uri[96];               /* config.json only ("wifiSettingsUri"): the system page the Wi-Fi settings button opens; "" = built-in default */
    int      nfavs;
    struct { char fp[65]; char alias[64]; } favs[SET_MAX_FAVS];
} ls_settings;

/* Reads config.json from dir (missing file / fields -> defaults). */
void settings_load(const char *dir);
void settings_snapshot(ls_settings *out);           /* consistent copy for reading */
void settings_save(void);

void settings_set_alias(const char *alias);
void settings_set_port(uint16_t port);
void settings_set_receive_dir(const char *dir);
void settings_set_auto_accept(int mode);
void settings_set_on_exists(int mode);
void settings_set_auto_install(int mode);
void settings_set_sound(bool on);
void settings_set_show_battery(bool on);
void settings_set_show_clock(bool on);
void settings_set_screensaver(int seconds);
void settings_set_hide_ids(bool hide);
void settings_set_check_updates(bool on);
void settings_set_update_asked_day(int ymd);
void settings_set_theme(int theme);
void settings_wifi_uri(char *out, size_t cap);     /* the configured address of the Wi-Fi settings page ("" = default) */
void settings_set_pin(const char *pin);             /* digits only, "" clears */

bool settings_is_fav(const char *fingerprint);
bool settings_toggle_fav(const char *fingerprint, const char *alias);   /* returns the new state */

#endif
