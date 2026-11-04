# Development log

How VitaSend grew, phase by phase. The numbers in brackets (v0.7.0xN) are the internal build numbers used while it was
developed; the first public release is **v0.9.0** (see [CHANGELOG](../CHANGELOG.md)).

| Phase | What | Status |
|---|---|---|
| 1 | Toolchain, hello-world VPK, mbedTLS built for the Vita, self-tests | done |
| 2 | Multicast discovery, `GET /info`, `POST /register` over HTTPS on port 53317 | done |
| 3 | Receive files (streamed to the memory card), accept/reject dialog, progress, cancel | done |
| 4 | Send files and whole folders, file browser, device picker, progress, cancel | done |
| 5 | Settings screen, history, favorites, PIN, text messages (both directions), on-screen keyboard | done |
| 6 | New graphical interface in the style of the desktop app (tabs, cards, icons, button symbols), real fonts with accents / Cyrillic / Greek | done (v0.7) |
| 7 | **.vpk installer** (like VitaShell's) + a **Files** tab; a received `.vpk` can be installed with TRIANGLE | done (v0.7.0x1) - **installer not run on a real Vita yet** |
| 8 | Setting to hide this Vita's IP address and fingerprint on screen (v0.7.0x2) | done |
| 9 | Renamed to **VitaSend**, new bubble + PS Vita icon, second colour theme **OLED** (pure black) in Settings (v0.7.0x3) | done |
| 10 | **Front touch screen** support (tap, drag-to-scroll) next to the buttons; the back panel is not used (v0.7.0x4) | done - works on a real Vita |
| 11 | App folder renamed to `ux0:data/VitaSend` (old `LocalSendVita` folder is moved over automatically) (v0.7.0x5) | done - **not run on a real Vita yet** |
| 12 | "by trashman" credit next to the version number (left menu and start-up screen) (v0.7.0x6) | done |
| 13 | Resume: **Retry** a failed or refused send with only the files that did not arrive (v0.7.0x7) | done |
| 14 | Setting for received files that already exist: keep both / skip / overwrite (v0.7.0x7) | done |
| 15 | **Files tab**: tick several files/folders and send them together, options menu with rename, delete, new folder, free space (v0.7.0x7) | done - **not run on a real Vita yet** |
| 16 | **Install queue**: several .vpk files (received together, or ticked in the Files tab) are installed one after another after a single question (v0.7.0x8) | done - **not run on a real Vita yet** |
| 17 | Optional **automatic installation** of received .vpk files (Off / Favorites only / Everyone; off by default) (v0.7.0x8) | done - **not run on a real Vita yet** |
| 18 | **Files tab keys swapped**: X opens a folder / ticks a file, SQUARE installs a .vpk (or ticks a folder) (v0.7.0x9) | done |
| 19 | **Battery percentage** top right on every page (v0.7.0x9) | done - **real battery not read on a Vita yet** |
| 20 | **Settings grouped under headers** (This device / Receiving / Display and sound / About) with an **About box** (version, version date, credit); Quick Save moved off the Receive page (v0.7.0x9) | done |
| 21 | Quick Save and *Install received .vpk files* are chosen from a **dropdown** and confirmed with what they do and what they risk (v0.7.0x9) | done |
| 22 | **Finished chime** for downloads and installations, can be switched off (v0.7.0x9) | done - **sound not heard on a real Vita yet** |
| 23 | **Screensaver**: bouncing VitaSend logo with transfer progress and battery, wakes on button/touch, requests, messages and finished transfers; Off / 30 s / 1 / 2 / 5 min (v0.7.0x9) | done - **not run on a real Vita yet** |
| 24 | Slower screensaver drift, a **clock** next to the battery on it; the chime plays after **every step** of a manual download + install, but only **once at the very end** when automatic installation does it (v0.7.0x10) | done |
| 25 | **D-pad column switching** (LEFT = tab list, UP/DOWN change tab, RIGHT/X/O back) is now a permanent part; Receive starts in the tab list and its *This device* block opens a detailed device overview with X; **battery** and **clock** are optional settings (not on the screensaver any more); every setting with more than two options is a **dropdown**; **Square** on the "Installed" screen opens the app; screensaver fixes (no instant switch-off after an installation, one wake-up at the end of a whole download + install chain, the waking touch / button does nothing else); Settings rows no longer overlap the scrollbar (v0.7.0x11) | done |
| 26 | **Tidy-up, no new behaviour:** `app.c` (2,800 lines) split into seven files by topic (see *Source layout*); small polish: "Press X for details" on the Receive card, a how-to on the empty Receive page, one "Close" in the device details, a hint for the battery / clock rows; a ready place in *Settings > About* for a "support the developer" line (`LS_SUPPORT_TEXT` in `src/util.h`, empty = not shown) (v0.7.0x12) | done |
| 27 | **Fixes:** the screensaver now always wakes when an installation ends (also a slow one; the input is re-armed after the system installer ran, and the log shows `installation running / idle`); the start-up image (LiveArea) has the text higher and centred on the logo so the system's Start marker no longer covers it; author credit (`trashman`) in every source file (v0.7.0x13) | done |
| 28 | **Screensaver wake fix (found from a real Vita log):** the idle check subtracted a time that was read *before* the wake-up stamped the idle timer, so the difference wrapped around and the screensaver switched on again 7 ms after waking. Guarded (and the same pattern in the peer list); a test now simulates slow input reading (v0.7.0x14) | done |
| 29 | **Chime rules simplified:** no more chime for "unpacked" (the one at 95 %); a chime when the download has finished and one when the installation has finished (once per queue); with automatic installation only the end of the installation chimes (v0.7.0x15) | done |
