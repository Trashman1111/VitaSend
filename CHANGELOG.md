# Changelog

All notable changes to VitaSend. The format follows [Keep a Changelog](https://keepachangelog.com/); versions follow
[Semantic Versioning](https://semver.org/) once 1.0 is reached (until then minor versions may change behaviour).

How the project got here, step by step, is in [docs/DEVELOPMENT_LOG.md](docs/DEVELOPMENT_LOG.md).

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

[0.9.0]: ../../releases/tag/v0.9.0
