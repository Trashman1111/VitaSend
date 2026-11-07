# Changelog

All notable changes to VitaSend. The format follows [Keep a Changelog](https://keepachangelog.com/); versions follow
[Semantic Versioning](https://semver.org/) once 1.0 is reached (until then minor versions may change behaviour).

How the project got here, step by step, is in [docs/DEVELOPMENT_LOG.md](docs/DEVELOPMENT_LOG.md).

## [0.9.7] - 2026-11-07

### Fixed
- **Self-update: "The system refused the package (0x80101114)".** The system installer cannot replace an app while it is running. The update now works like VitaShell's: VitaSend unpacks the new version, installs a tiny **updater app** (a copy of its own program under the title id `LSNDUPD01`, shown as "VitaSend" until removed), and closes. The updater stops everything else, installs the new version, and starts VitaSend again; at the next start VitaSend reports the result in the log (`Update: the updater app installed the new version ...` / `Update: FAILED - <reason>`) and removes the updater app. The screen goes dark for a moment; nothing needs to be pressed. Hardware-untested: if it fails, the log says why and the downloaded package in `ux0:data/VitaSend/update/` can still be installed with VitaShell.
- Test: the "accept / reject a request" steps no longer fail on a slow machine when the answer key arrives in the very frame the dialog appears (seen once on GitHub's runner).

### Changed
- The installer now writes its result to the log (`Install: done - ...` / `Install: FAILED - <reason, with the system's error code>`), so a failed installation can be diagnosed from the log alone.

## [0.9.6] - 2026-11-07

### Changed
- Version bump only: this release exists to try the new in-app update (0.9.5 offers it, downloads it, checks it and installs it). No other changes.

### Fixed
- The automated tests no longer contact the real GitHub (every test instance now asks a dead local port unless a test sets up its own fake server), so a newer release can never change a test result.

## [0.9.5] - 2026-11-06

### Added
- **Update check and self-update** (`src/update.c`, `src/app_update.c`). A few seconds after the network is there, VitaSend asks GitHub (`Trashman1111/vitasend`) for the newest release. **Pre-releases count**; drafts do not; the highest version number wins. A new version is offered in a window (**X Download and install / O Not now**) **at most once a day** (the day is stored in `config.json` as `updateAskedDay`) and **never in the middle of something**: not during a transfer, a request, an installation, a copy / delete / unzip, a dialog, or the screensaver. If nothing newer exists nothing is shown.
- **Settings > Display ... > Check for updates at start** (On by default; **X** switches it). Off = GitHub is only asked when you press the button below.
- **Settings > About > Check for updates** (button, or **X** on the About box): a manual check that always answers - *VitaSend is up to date*, the new version, or why it failed (with **SQUARE = Try again**).
- **The download is checked.** The file must match the SHA-256 in the release's `SHA256SUMS.txt` (or, if there is none, the digest GitHub lists for the file); a release without any checksum is refused. The package then goes through the normal installer (`Install the new VitaSend?`); afterwards **SQUARE closes VitaSend** so you can start the new version. A downloaded file is kept in `ux0:data/VitaSend/update/` (install it with VitaShell if the in-app install ever fails).
- **Verified HTTPS.** The update connection checks the server's certificate chain and host name against **25 root certificates built into the program** (Mozilla's list, `src/cacerts.c`, made by `tools/gen_cacerts.py`; MPL-2.0 data, see THIRD_PARTY_NOTICES). The mbedTLS build has no clock, so certificate **dates are not checked** (a stolen, expired certificate would still be accepted; the SHA-256 check of the download is independent of that).
- **Deleting shows a progress bar.** Deleting files and folders runs in a dialog that counts files and folders and moves like the copy dialog does; **O = Stop** (what is already deleted stays deleted).
- **Unzip.** **X** on a `.zip` file in the Files tab (or *Unzip ...* in the SELECT menu) asks *Unzip this file?* (number of files, size, the new folder's name), unpacks it into a **new folder next to the file** named like it (an existing name gets "(1)"), with a progress bar and **O = Cancel**. Stored and deflated files only (no passwords, no ZIP64); names with `..`, absolute paths or too-deep nesting are refused **before anything is written**; a damaged file or a full card removes the new folder again (all or nothing). The zip file stays.

### Changed
- **After waking the Vita, VitaSend waits up to 5 seconds for Wi-Fi** ("Waiting for Wi-Fi...") before showing the *Wi-Fi is turned off* window, because the Vita needs a moment to bring Wi-Fi up after sleep. (At start only; the *No Wi-Fi* card on the Receive page after a connection is lost is unchanged.)
- **Settings order**: *Install received .vpk files* now sits directly under *Quick Save*; *When a received file already exists* follows.

### Notes
- The in-app self-update is new and was tested on the PC build only (a fake GitHub with its own certificate authority). If the install over the running app does not work on your Vita, the log (`Update: ...`) says where it stopped and the downloaded package is in `ux0:data/VitaSend/update/`.

### Tested
- New automated tests: `--only update` (47 checks: start-up check, pre-releases, drafts, 0.10.0 above 0.9.9, once-a-day rule, switch, manual check, redirect + chunked download, checksum from the sums file / from the digest / missing / wrong, unknown authority, wrong host name, server down, never interrupting a request) and `--only fileops` (34 checks: delete progress and stopping, unzip with progress, safety, damaged files, cancel); Wi-Fi start delay (`--only wifi`); settings order (`--only polish`).

## [0.9.4] - 2026-11-06

### Fixed
- **The Wi-Fi settings button went one page too far** (it opened *Configure Access Points*). It now asks the system for the Wi-Fi settings page itself (`settings_dlg:wifi`). That address is not on the community's list of known addresses (only `wifi::wps` and `wifi::manual` are), so it is a best guess; if it opens something else, put another one into `config.json` (`"wifiSettingsUri": "settings_dlg:..."`, VitaSend must be closed while editing) - no new build needed. If the system refuses the address, the plain Settings list (`settings_dlg:`) opens instead. `log.txt` shows `Wi-Fi: launching ... returned 0x...`.

### Notes
- Switching Wi-Fi on from inside the app does not work on firmware without the `sceWlanSetConfiguration` call (the log says *this firmware has no such call*); there the settings button is the way.

## [0.9.3] - 2026-11-06

### Added
- **Wi-Fi settings button.** When switching Wi-Fi on from the app is refused (the author's firmware has no such system call), the *Wi-Fi could not be turned on* window now offers **TRIANGLE = Wi-Fi settings**, which opens the Vita's own settings (VitaSend closes; start it again afterwards). The Receive card's button becomes **Wi-Fi settings** in the same situation. If the settings cannot be opened, the window says so.
- **Viewer for pictures and text files** (`src/app_view.c`, with the public-domain `stb_image` v2.30). **JPEG, PNG, BMP and GIF** are shown whole and scaled to the screen, JPEG photos turned the right way up (EXIF orientation); **text files** (txt, md, log, json, xml, ini, csv, source files, ...) are shown wrapped and scroll (the first 128 KB). **L / R**, **LEFT / RIGHT** or a tap on the left / right third go to the previous / next file; **O** closes. Pictures that would not fit in memory (more than 10 MP, progressive JPEG over 2.5 MP, other formats over 6 MP) are refused with a message.
- **File queues.** Tick several files in the Files tab and press **X**: pictures and text files open as one queue you can page through; packages open one **Install these apps?** question with the list. Ticked folders and files that cannot be opened are left out (a message says so); packages and pictures cannot be mixed.

### Changed
- **Files tab keys**: **X** is the main button (open a folder, install a `.vpk`, view a picture or text file); **SQUARE** is now only *tick / untick*; **TRIANGLE** sends. The hint bar shows what X does for the highlighted file (*Open*, *Install*, *View*, *Install ticked*, ...).
- The Files **SELECT** menu no longer repeats what has a button of its own (Install, Tick, Send); it keeps tick / untick all, copy, move, rename, delete and new folder.
- The package question for several packages is titled **Install these apps?** (one package: *Install this app?*).
- The **LR Tabs** hint is shown on the Receive tab only; L / R switch tabs everywhere as before.

### Tested
- New automated tests (`--only view`): hints, picture colours, EXIF rotation, PNG transparency, refused pictures, text scrolling and long lines, a binary file, queues of pictures / texts / packages, the slimmer menu, the hint bar of every tab; Wi-Fi settings button (`--only wifi`).

### Please test
- Opening the Wi-Fi settings (`settings_dlg:wifi::manual`, then `settings_dlg:`) could not be tried on a console; `log.txt` shows `Wi-Fi: launching ... returned 0x...`. Please send that line if nothing opens.
- How long a big JPEG takes to decode on the Vita is unmeasured; the viewer shows *Loading...* and the buttons wait until it is done.

## [0.9.2] - 2026-11-06

### Added
- **Wi-Fi switched off? Turn it on without leaving the app.** If Wi-Fi is off when VitaSend starts, a dialog asks: *Wi-Fi is turned off - VitaSend needs Wi-Fi to find other devices.* **X** turns it on (with a progress state and **O** to cancel), **SQUARE** *Continue without Wi-Fi* starts the app offline (Files, History, Settings all work; the server and discovery start by themselves when Wi-Fi comes later), **O** quits. If the system does not allow it from inside an app, the dialog says so and points to *Settings > Network*; VitaSend keeps waiting and continues by itself as soon as Wi-Fi is there.
- **Wi-Fi lost while the app runs**: no pop-up. The Receive card shows an amber **No Wi-Fi** chip and *Wi-Fi not connected*, with a **Turn on Wi-Fi** button (**TRIANGLE**, also while the tab list has the focus, or tap it). The sidebar's bottom row becomes a tappable *No Wi-Fi* with a struck-through symbol. While it connects the card shows *Connecting* and *Turning on Wi-Fi...*; when the network is back it returns to *Ready* and announces itself again.

### Please test
- The switch-on request uses an undocumented system call (`sceWlanSetConfiguration`, the one vitacompanion uses), because Sony offers no official way for apps. It could not be tried on a real console while writing this: the system's answer is written to `log.txt` (`Wi-Fi: sceWlanSetConfiguration(1) returned 0x...`). If it does nothing on your firmware, the fallback dialog appears after 20 seconds - please report the log line.
- A build without the `SceWlanBt_stub_weak` library (older VitaSDK) still works; the button then answers "not possible here".

### Tested
- New automated tests (`--only wifi`): Wi-Fi off at the start (ask, switch on, cancel, refuse, no network in time, continue offline, quit), Wi-Fi lost and regained while running, touch on the sidebar row.

## [0.9.1] - 2026-11-05

### Added
- **Copy** and **Move** in the Files tab. *SELECT > Copy* / *Move* marks the ticked items (or the highlighted one); open the destination folder and press **TRIANGLE** (or tap **Copy here** / **Move here**). A copy never overwrites: an existing name becomes `name (1).ext`, also when copying into the same folder.
- **Move**: inside one memory card the item is renamed, which is instant. Between two cards the data is copied first and the original is removed only after the copy of that item is complete, with a progress dialog and **O** to cancel; a failed or cancelled move never loses data. Nothing is ever overwritten (an existing name is skipped and reported), a folder cannot be moved into itself, and VitaSend's own data folder cannot be moved.
- The Files options menu scrolls when it has more than nine entries.

### Tested
- Works with the LocalSend apps for Android, Windows, Linux and iPhone (reported by the author's own tests).
- New automated tests for Copy and Move (`--only move`): rename, several items, name conflicts, folder into itself, cancel, a copy between cards with sub-folders, cancel during a big copy, not enough free space, touch.

## [0.9.0] - 2026-10-29 - first public release

A beta: everything below works and is covered by the automated tests on a PC; the author has used it on a real PS Vita
together with the LocalSend desktop app. Expect rough edges on other hardware and firmware - please report them.

### Features
- **Receive and send files, folders and text** with any LocalSend v2.1 device on the same Wi-Fi (discovery, HTTPS, PIN, favourites, Quick Save, retry of failed sends).
- **Graphical interface** in the style of the desktop app: Receive, Send, History, Files and Settings tabs, dark and OLED themes, PlayStation button hints, accents / Greek / Cyrillic text, on-screen keyboard.
- **Full control by buttons, D-pad and the front touch screen** (tap, drag to scroll).
- **Files tab**: browse the memory cards, tick several files, send, rename, delete, new folder.
- **`.vpk` installer** (VitaShell procedure): single packages and queues, optional automatic installation of received packages, *Open app* after installing.
- **Screensaver** with transfer / installation progress that wakes on button, touch, requests and finished jobs.
- **Settings**: device name, PIN, hide IP and fingerprint, receive folder, Quick Save, install mode, what to do with existing files, theme, battery indicator, clock, screensaver time, finished chime.
- **Device details** page and a log viewer.

### Fixed during the last development builds
- The screensaver restarted itself right after waking when the main loop read the time before the wake-up stamped the idle timer (unsigned wrap-around).
- The long fingerprint overlapped the "Press X for details" hint on the Receive page.
- The LiveArea start image: text is centred on the logo and higher, so the system's Start marker no longer covers it.
- Chimes: one when a download is finished, one when an installation is finished (once per queue); none in between.

[0.9.2]: ../../releases/tag/v0.9.2
[0.9.1]: ../../releases/tag/v0.9.1
[0.9.0]: ../../releases/tag/v0.9.0
