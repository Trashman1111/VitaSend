# About this port

[LocalSend](https://localsend.org) is a free, open-source app for sending files between devices on the same network. Its
official apps are built with Flutter, which does not exist for the PS Vita. VitaSend is therefore not a *port* of the
LocalSend code - it is a **native re-implementation of the LocalSend protocol** for the Vita, written in C from the public
[protocol specification](https://github.com/localsend/protocol), so that the Vita can talk to every official LocalSend app.

This page explains how it works, which tools it is made with and which Vita-specific problems shaped the design. For the
code map see [ARCHITECTURE.md](ARCHITECTURE.md); for the wire protocol see [PROTOCOL.md](PROTOCOL.md).

## How it works, in one minute

1. **Find each other.** The Vita announces itself on the multicast address `224.0.0.167:53317` and listens for others.
   Whoever announces gets a `POST /register` back with the Vita's identity (alias, model, fingerprint, port).
2. **Secure connection.** The Vita runs an HTTPS server (TLS 1.2) with a self-signed ECDSA P-256 certificate that it creates
   on first start. The SHA-256 of that certificate is the device's *fingerprint* - its identity in the LocalSend world.
3. **Ask, then transfer.** A sender calls `POST /prepare-upload` with the list of files. The Vita shows who is sending what
   and waits for X or O (or accepts by itself if *Quick Save* says so). Each accepted file then arrives with its own
   `POST /upload`, streamed straight to the memory card as `name.part` and renamed when complete.
4. **The other direction** uses the same calls in reverse: the Vita asks the other device with `prepare-upload` and streams
   the files out.
5. **Installing.** A received `.vpk` is a ZIP file. VitaSend unpacks it, writes the `head.bin` the system wants and hands the
   folder to the Vita's own installer, which creates the LiveArea bubble - the same procedure VitaShell uses.

## Tools and libraries

| Tool | Used for |
|---|---|
| **[VitaSDK](https://vitasdk.org/)** (`arm-vita-eabi-gcc`, newlib, `vita.cmake`, stub libraries) | building the SELF / VPK; the Vita system APIs (sceNet, sceIo, sceCtrl, sceTouch, sceDisplay, sceKernel, ScePower, ScePromoterUtil, ...) |
| **[mbedTLS](https://github.com/Mbed-TLS/mbedtls) 3.6.2** | TLS 1.2, X.509 / ECDSA, SHA-256, DRBG. Built by `scripts/build-mbedtls.sh` with the modules the Vita cannot use switched off |
| **CMake** | the build; detects optional SDK libraries |
| **[stb_truetype](https://github.com/nothings/stb)** | rasterising the embedded fonts |
| **[stb_image](https://github.com/nothings/stb)** | decoding pictures for the viewer (JPEG, PNG, BMP, GIF) |
| **[certifi](https://github.com/certifi/python-certifi)** (Mozilla's root list, MPL-2.0) | the root certificates built into the update check (`tools/gen_cacerts.py` writes `src/cacerts.c`) |
| **Poppins, Liberation Sans** (SIL OFL) | the UI fonts, cut down and compiled into the program by `tools/gen_fontdata.py` |
| **[vita-headers](https://github.com/vitasdk/vita-headers)** | checking the Vita-only files compile against the real API without a full SDK build |
| **[VitaShell](https://github.com/TheOfficialFloW/VitaShell)** (source, GPL-3.0) | reference for the package installer: the `head.bin` format and the PAF / ScePromoterUtil procedure |
| **Python 3 + openssl** | the test scripts (they play the other LocalSend device and press the buttons), icon / LiveArea generation (Pillow) |
| **AddressSanitizer / UBSan** | every test run of the PC build |
| **GitHub Actions** | CI and release builds |

Everything else - HTTP/1.1 server and client, JSON, ZIP and DEFLATE, the 2D renderer, the widgets, the touch handling - is
written for this project and has no dependency besides the C library.

## Vita-specific problems and the decisions they led to

**TLS without an operating system's help.** mbedTLS expects `/dev/urandom`, BSD sockets and a clock. The Vita offers none
of them in that form, so the build script switches off `net_sockets`, `timing` and the platform entropy, and
`src/tls.c` feeds mbedTLS from the Vita's hardware random number generator. Generating the ECDSA key takes a few
milliseconds; the whole TLS 1.2 handshake in memory about 140 ms, which is why a **self-test at every start** checks random
numbers, SHA-256, the certificate and a loopback handshake before the network is touched.

**The network stack has moods.** `sceNet` returns different error codes than BSD sockets and some firmware versions refuse
calls that others accept. `plat_vita.c` therefore tries several ways of creating, binding and listening on each socket,
remembers the exact error codes and shows them in the log; if port 53317 is busy (for example a copy of the app that was
not closed properly) it uses the next free one and announces that port.

**No zlib, no spare memory.** `.vpk` files are ZIP archives, and homebrew games are big. VitaSend has its own ZIP reader and
DEFLATE decoder (`zip.c`, `inflate.c`, checked against Python's `zlib` on many inputs, including truncated and corrupted
streams) and always **streams**: uploads and downloads go straight between the socket and the card in small chunks, the
`.vpk` is unpacked file by file, and the heap is fixed at 64 MB.

**A user interface with only a CPU and two framebuffers.** Everything is drawn by software into one 960 x 544 picture and
copied to the screen once per frame: anti-aliased rounded rectangles and circles, UTF-8 text with accents, Greek and
Cyrillic, icons and the PlayStation button symbols drawn from line data (no image files). Dialogs cache their dimmed
background, and the frame rate adapts (30 fps with the screensaver, 8 fps while something is moving, 2 fps when idle, and an
immediate redraw after any input).

**Buttons *and* touch from one description.** Widgets register tap areas while they are drawn, so every screen works with
the D-pad, the buttons and the front touch screen without separate code paths. Touch handling (tap vs drag, scaling from the
panel's 1920 x 1088 to the 960 x 544 picture) is portable and tested with simulated fingers.

**Installing apps needs more permissions than apps normally have.** The system installer (`ScePromoterUtil`) can only be
loaded by an application with *extended permissions*, so VitaSend is built as an "unsafe" SELF, exactly like VitaShell. That
is a real trade-off - the user must enable *unsafe homebrew* - and is therefore a CMake option (`LS_UNSAFE`); the app is
usable without it, just without the installer.

**Power management.** The Vita dims and sleeps quickly. The app keeps itself awake while it is open
(`sceKernelPowerTick`) and has its own **screensaver** (black screen, a slowly bouncing logo, progress of the running
transfer) so a long transfer does not need a bright, static picture on the display (a black screen with a moving logo is also gentler on an OLED panel).

**Time bugs only a real console shows.** A subtle unsigned-integer wrap-around in the idle timer made the screensaver switch
itself on again a few milliseconds after waking. The PC build never showed it because reading input takes no time there;
the log of a real Vita did. The test harness now has "slow call" knobs (`LS_SLOW_INPUT_MS` and friends) to reproduce this
class of bug on a PC.

## How it is tested

Because only two files touch the Vita, the rest of the program - protocol, engines, installer and the entire UI - is compiled
for Linux and run under AddressSanitizer against scripts that **play the other LocalSend device** and **press the buttons**
(and put simulated fingers on the touch screen). More than 600 checks cover the transfer edge cases (cancel from both sides,
dropped connections, path traversal, checksum errors, 120 files in one request, PIN lock-out ...), the installer (corrupt and
malicious packages) and the interface. What the tests cannot cover - the real installer, Wi-Fi, touch panel, power - is
verified by hand on a real Vita. See [TESTING.md](TESTING.md).

## What is not done (yet)

Parallel transfers, IPv6, the browser download API, languages other than English, scripts other than Latin / Greek /
Cyrillic. See [CONTRIBUTING.md](../CONTRIBUTING.md) for how to help.
