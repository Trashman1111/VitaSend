# Architecture

This document is the map for anyone who wants to read or change the code. It explains how the program is put together,
which thread does what, where each feature lives, and the conventions the code follows. Every source file also starts with
a comment saying what it is for - this page ties them together.

* [The big picture](#the-big-picture)
* [Layers and the platform boundary](#layers-and-the-platform-boundary)
* [Threads and shared state](#threads-and-shared-state)
* [Source map](#source-map)
* [Network: discovery, HTTP, TLS](#network-discovery-http-tls)
* [Receiving and sending](#receiving-and-sending)
* [The .vpk installer](#the-vpk-installer)
* [The user interface](#the-user-interface)
* [Data on the memory card](#data-on-the-memory-card)
* [Conventions](#conventions)
* [Where to start for common changes](#where-to-start-for-common-changes)

## The big picture

```
                     +-------------------------------- PS Vita --------------------------------+
  other LocalSend    |                                                                          |
  device  <--UDP---> |  discovery.c  (announce / listen, register workers)       ---> peers.c   |
          <--HTTPS-> |  http.c + tls.c + api.c   (server: /info /register /prepare-upload ...)  |
                     |        |                                                                  |
                     |        +--> rx.c  receive: ask the user, stream to <file>.part, rename    |
                     |  send.c  send:    prepare-upload on the other device, stream uploads      |
                     |        |                                                                  |
                     |        v            progress                     snapshots                 |
                     |     xfer.c  <------------------------------------------+                  |
                     |                                                          |                  |
                     |  app.c (UI thread = main thread)  --- app_*.c pages ---> gfx.c --> screen  |
                     |        |   buttons + touch <-- ui_vita.c                                   |
                     |        +--> vpk.c  (unpack with zip.c/inflate.c, head.bin, promote)        |
                     +--------------------------------------------------------------------------+
```

VitaSend implements the [LocalSend protocol v2.1](https://github.com/localsend/protocol) in plain C (see
[PROTOCOL.md](PROTOCOL.md)). It is a normal homebrew application: one `eboot.bin` (built as an "unsafe" SELF because the
installer needs the system's `ScePromoterUtil`), no plugins, no kernel code. The only libraries are the VitaSDK stubs and
**mbedTLS 3.6** (built by `scripts/build-mbedtls.sh`). Everything else - HTTP, JSON, ZIP/DEFLATE, the 2D renderer, the
widgets - is in this repository.

## Layers and the platform boundary

```
  app*.c  ui.c  gfx*.c  touch.c          user interface (portable C)
  rx.c  send.c  xfer.c  vpk.c  ...        engines (portable C)
  api.c  http.c  tls.c  discovery.c       network protocol (portable C)
  json.c  util.c  zip.c  inflate.c ...    helpers (portable C)
  ---------------- plat.h ----------------  <- the ONLY interface to the operating system
  plat_vita.c  ui_vita.c                  PS Vita (VitaSDK: sceNet, sceIo, sceKernel, sceCtrl, sceTouch, ScePromoterUtil)
  plat_posix.c ui_host.c                  PC test build (BSD sockets, pthreads, files; screen to a file, scripted buttons)
```

**`src/plat.h` is the contract.** It declares threads, mutexes, files and directories, time, random numbers, UDP and TCP
sockets, battery and clock, the installer hand-over (`plat_promote`) and a few more. `ui.h` declares the matching screen /
input functions (`ui_present_frame`, `ui_buttons`, `ui_touch`). Two implementations exist:

| | PS Vita | PC test build (`host/`) |
|---|---|---|
| platform | `plat_vita.c` | `plat_posix.c` |
| screen + input | `ui_vita.c` (2 framebuffers, `sceCtrl`, `sceTouch`) | `ui_host.c` (screen dumped as text / PPM picture, buttons and fingers read from a pipe) |

Nothing else in `src/` calls a Vita function or includes a Vita header, so everything above the line compiles and runs on a
PC - that is how the project is tested without a console (see [TESTING.md](TESTING.md)). If you add a new OS feature,
declare it in `plat.h`, implement it in both `plat_*.c` files (the PC version can be a stand-in), and keep Vita headers out
of everything else.

## Threads and shared state

| Thread | Started by | What it does |
|---|---|---|
| **main / UI** | the system | `app_run()` in `app.c`: reads buttons and touch every ~30 ms, handles events, draws (33 ms with the screensaver, 120 ms while something is moving, 500 ms when idle - and at once after any input) |
| **discovery** | `discovery_start()` | UDP socket on `224.0.0.167:53317`: sends the announcement burst and then every 30 s, listens for other devices |
| **register workers** (max 3) | discovery | `POST /register` to a device that announced itself (short-lived) |
| **HTTP accept** | `http_server_start()` | accepts TLS connections (non-blocking listener) |
| **connection threads** (max 6) | HTTP accept | one per connection: parse a request, call `api_handle()`; for `/upload` the body is streamed |
| **send worker** | `send_start_files()` / `send_start_text()` | one sending job: prepare-upload, then one upload per file |
| **install worker** | `vpk_start()` | unpack and install one `.vpk` |

The rules that keep this simple:

* **The UI thread owns the screen and all `app_*.c` state** (`app_state A` in `app_int.h`). No other thread touches it.
* **Other threads publish, the UI thread reads snapshots.** Progress lives in `xfer.c` (`xfer_snapshot`), the installer in
  `vpk.c` (`vpk_snapshot`), settings in `settings.c` (`settings_snapshot`), nearby devices in `peers.c`, history in
  `history.c`. Each of these modules has its own mutex; callers get a *copy* of the data.
* **Questions go the other way through small polled handshakes.** A request from the other device blocks its HTTP thread in
  `rx_handle_prepare()` until the user has answered; the UI thread notices it with `rx_pending()` and answers with `rx_decide(accept)`. A PIN prompt for sending works the
  same way (`send_needs_pin()` / `send_give_pin()`).
* **Only one transfer at a time.** `xfer.c` holds the single running transfer; a second sender gets HTTP 409 "busy". While
  an installation runs, incoming transfers are answered "busy" too.
* **Threads are counted** (`ls_spawn` / `ls_wait_threads` in `util.c`) so shutdown can wait for them.

## Source map

### Network and protocol
| File | Purpose |
|---|---|
| `api.c` | the REST routes (`/info`, `/register`, `/prepare-upload`, `/upload`, `/cancel`); everything else answers 404 / 403 |
| `http.c` / `http.h` | HTTP/1.1 server and client on top of `conn_t`: request parsing, `Content-Length` and chunked bodies, `Expect: 100-continue`, streaming bodies to a handler |
| `tls.c` / `tls.h` | all mbedTLS use: hardware random numbers, the device certificate (ECDSA P-256, created on first start), `conn_t` = one HTTPS (or plain HTTP) connection, SHA-256 |
| `discovery.c` | multicast announcements, listener, register workers, UDP fallback reply |
| `peers.c` | the thread-safe list of nearby devices (max 48) |
| `identity.c` | alias / model / fingerprint and the JSON the protocol wants them in |
| `json.c` / `json.h` | a small JSON parser (tree of `json_val`) and writer (`jw`) |

### Transfers and installer
| File | Purpose |
|---|---|
| `rx.c` | receiving: `prepare-upload` (asks the user or applies Quick Save, checks PIN, free space, file names), `upload` (writes `name.part`, renames when complete, checks SHA-256 if given), `cancel` |
| `send.c` | sending: collect files and folders, `prepare-upload` on the other device (PIN, 401/403/409/429), upload each accepted file, cancel, retry |
| `xfer.c` | the one running transfer and its progress (speed smoothing, serial number) |
| `vpk.c` | `.vpk` installer: inspect, unpack with progress and cancel, `head.bin`, hand to the system |
| `unzip.c` | unpacking an ordinary `.zip` into a new folder in a thread (all entries checked first: safe names, depth, stored / deflate only; free space; CRC; a failure or cancel removes the folder) |
| `update.c` | update check and download in threads: its own HTTPS GET (verified TLS, redirects, chunked / Content-Length), GitHub releases JSON (pre-releases count), SHA-256 from `SHA256SUMS.txt` or the asset digest, file in `<data>/update/`; never installs anything itself (the self-update is `vpk_start_self_update` in `vpk.c`: unpack + head.bin into `ux0:data/pkg`, install a helper app - this program's own eboot.bin and param.sfo with the title id `LS_HELPER_TITLE_ID` = `LSNDUPD01` - then `app_install.c` launches it; in the helper, `main()` calls `plat_update_helper_run()`, which stops other apps, promotes the package, writes `data/update-result.txt` and relaunches VitaSend; the next start of VitaSend reads the result, logs it and deletes the helper) |
| `cacerts.c` | the 25 root certificates the update connection trusts (generated by `tools/gen_cacerts.py` from Mozilla's list); `tls.c` verifies the chain and host name against them (no clock: dates are not checked) |
| `zip.c`, `inflate.c` | ZIP central-directory reader and DEFLATE decoder (no zlib), CRC checked |
| `headbin.c` | builds `sce_sys/package/head.bin` (template and hash layout from VitaShell - this is why the project is GPL-3.0) |

### Settings and data
| File | Purpose |
|---|---|
| `settings.c` | `config.json`: alias, port, receive folder, Quick Save, install mode, exists-mode, PIN, favorites, theme, battery / clock / screensaver / sound |
| `history.c` | `history.json`: the last 40 transfers and text messages |
| `util.c` | logging (ring buffer + `log.txt`), threads, string / path / file helpers |

### User interface
| File | Purpose |
|---|---|
| `app.c` | main loop, tab keys, button / touch input, events from other threads, drawing dispatch, battery / clock / chime helpers |
| `app_pages.c` | Receive (with the "This device" details), Send, History |
| `app_files.c` | file browser for sending and the Files tab (with its options menu: copy, move, rename, delete, new folder, ...) |
| `app_settings.c` | Settings rows, About, log viewer, dropdown lists, setting confirmations |
| `app_dialogs.c` | on-screen keyboard, message viewer, incoming request, transfer progress, confirmations |
| `app_view.c` | the viewer: pictures (JPEG / PNG / BMP / GIF through `stb_image.h`, EXIF orientation, box-filter scaling to 960x456) and text files (first 128 KB, wrapped); a queue of up to 16 files; called by `app_files.c` |
| `app_install.c` | install dialogs, the install queue, automatic installation, chimes |
| `app_move.c` | copying files / folders and moving them between memory cards: copy in small steps (a move then removes the original); progress dialog |
| `app_fileops.c` | deleting with a progress dialog (small steps from the main loop, explicit stack of directory listings, cancellable) and the unzip dialogs (ask, progress, result) on top of `unzip.c` |
| `app_update.c` | the update dialogs (checking, new version, download progress, failure) and the rules for the automatic check: 4 s after the IP appears, once a day (`updateAskedDay`), once per run, never over a transfer / request / dialog / screensaver |
| `wifi.c` | "Wi-Fi is off": asks the platform to switch it on and follows the request (idle / connecting / failed after 20 s); used by `main.c` (start-up dialog) and the Receive page; when the switch-on is refused, `wifi_user_action()` opens the Vita's Wi-Fi settings (`plat_open_wifi_settings`) |
| `app_saver.c` | the screensaver |
| `app_int.h` | what the `app_*.c` files share: screen ids, `app_state A`, the function list of each file |
| `ui.c` / `ui.h` | widgets and layout (sidebar, cards, chips, hint bar, dialogs, progress bars) and the **tap-area registry** for touch |
| `gfx.c`, `gfx_icons.c` / `gfx.h` | the 2D renderer: anti-aliased shapes, UTF-8 text, icons and PlayStation button symbols drawn from lines |
| `stb_image.h` | image decoder (v2.30, only JPEG / PNG / BMP / GIF, no SIMD), used by `app_view.c` only |
| `fontdata.c`, `stb_truetype.h` | the embedded fonts (generated by `tools/gen_fontdata.py`) and the rasteriser |
| `touch.c` | finger positions -> tap / drag (portable) |

### Platform
`plat.h`, `plat_vita.c`, `plat_posix.c`, `ui_vita.c`, `ui_host.c` - see [above](#layers-and-the-platform-boundary).
`main.c` is the program start (self tests, identity, server, discovery, then `app_run`).

## Network: discovery, HTTP, TLS

* **Discovery.** Multicast UDP `224.0.0.167:53317`. At start a burst of announcements, then every 30 s, and whenever the
  Wi-Fi address changes or the user presses TRIANGLE. A device that announces itself gets a `POST /register` from us (the
  protocol's answer); if that fails we reply over UDP with `announce:false`. Both the v2 (`announce`) and v1
  (`announcement`) field names are sent and understood.
* **HTTPS.** TLS 1.2, ECDHE-ECDSA with a self-signed P-256 certificate. The LocalSend *fingerprint* is the SHA-256 of that
  certificate, so it is the device's identity; peer certificates are not verified (the official apps behave the same).
* **HTTP.** `http.c` is a deliberately small implementation: one thread per connection, `Connection: close` after each
  response, bodies up to 256 KB buffered, bigger ones only through routes that ask to be streamed
  (`api_wants_stream()` - currently `/upload`). Sending uses the same code as a client, one connection per request.
* **Ports.** 53317 by default. If it is taken the next 9 ports are tried, then any free port; other devices learn the
  port from the announcement. The Vita's network library reports odd errors, so `plat_vita.c` tries several ways of
  opening each socket and keeps the exact codes for the on-screen log (`plat_net_error_text`).
* **Performance.** The CPU is set to 444 MHz at start (`plat_init`); TLS is the bottleneck, expect a few MB/s.

## Receiving and sending

**Receiving** (`rx.c`). `POST /prepare-upload` arrives on a connection thread. Checks first (busy? PIN? free space? sensible
names?), then either Quick Save accepts it or the thread **waits** for the UI to show the request dialog and call back.
Accepted files get a token each; every `POST /upload` writes into `<name>.part` (streamed in chunks straight from the TLS
connection), verifies SHA-256 if the sender provided one, and renames the file. File names are sanitised (no `..`, no
absolute paths) and an existing name is handled by the *When a received file already exists* setting.

**Sending** (`send.c`). The UI builds a list of items (`send_item`) from the file browser or a typed text, picks a peer and
starts the worker. The worker asks the other device (`prepare-upload`, with `?pin=` when needed), then uploads every
accepted file in turn, streaming from the card. Replies 200 (tokens, maybe for only some files), 204 (text delivered), 401
(PIN), 403, 409, 429 are all handled; *Retry* re-sends only the files that did not arrive.

**Progress** goes to `xfer.c` from both engines; the UI shows it in the transfer dialog, the sidebar card and the
screensaver.

## The .vpk installer

`vpk.c` does what VitaShell does, in four steps (each reports progress and can be cancelled while unpacking):

1. `vpk_inspect()` reads the ZIP directory and `sce_sys/param.sfo` and returns title, title id, version, size and whether
   the `eboot.bin` asks for extended permissions - without unpacking anything.
2. Unpack into `ux0:data/pkg` with `zip.c` / `inflate.c` (CRC-checked, names containing `..` or absolute paths refused,
   needs the unpacked size + 4 MB free).
3. Write `sce_sys/package/head.bin` (`headbin.c`) unless the package brings its own.
4. `plat_promote()` (Vita: load the system modules PAF and ScePromoterUtil, `scePromoterUtilityPromotePkgWithRif`, unload)
   - this adds the bubble. Then the scratch folder is deleted, also after a failure.

`app_install.c` owns the dialogs: a single package, or an **install queue** of up to 16 (one question, then one after the
other), and optional **automatic installation** of received packages. VitaSend refuses to install itself over itself
(title id `LSND00001`). On the PC build, `plat_promote` is a stand-in that checks the folder and copies it to
`<data>/installed/<TITLEID>`, which is what the tests inspect.

## The user interface

* **Rendering.** `gfx.c` draws everything into one 960 x 544 RGBA picture in ordinary memory (software, anti-aliased) and
  `ui_present_frame()` copies it to the display once per frame. Text is UTF-8 through stb_truetype with two embedded fonts
  (Poppins; Liberation Sans for Greek / Cyrillic / rare letters). Icons and the PlayStation button symbols are drawn from
  line data (`gfx_icons.c`), so there are no image assets.
* **Immediate mode.** Each frame `draw_screen()` (in `app.c`) redraws the whole screen for the current `screen_id`: a *tab*
  (Receive, Send, History, Files, Settings) with an optional *dialog* on top. Dialogs cache their dimmed background
  (`ui_dialog_begin`) so they are cheap.
* **One description for buttons and touch.** While drawing, widgets register **hit areas** (`ui_hit_*` in `ui.c`): a rect, a
  kind and a value. A tap finds the topmost hit area and is turned into the same event a button press would produce
  (`ui_touch_tap`). The bottom **hint bar** is built from a spec string such as `"X Send|SQ Files|L R Tabs"` with
  `ui_hints()` and is tappable too. This is why every feature works with both input methods without extra code.
* **Focus.** The D-pad moves a cursor inside the page; LEFT moves the focus to the tab list (`A.side_focus`), where UP / DOWN
  change the tab. `gfx_focus()` marks the focused element (it also writes `>` into the text dump of the PC build, which is
  how the tests know where the cursor is).
* **Input swallowing.** The press or touch that wakes the screensaver is swallowed so nothing is pressed by accident.
* **Settings with more than two options use a dropdown list** (`pk_def k_pk[]` in `app_settings.c`); two-option settings
  toggle with X. Risky choices (Quick Save, automatic installation) show a confirmation first.
* **Themes** are colour tables in `ui.c` (Dark and OLED).

## Data on the memory card

Everything is under `ux0:data/VitaSend/`: `config.json`, `history.json`, `cert.der` + `key.der` (device identity),
`log.txt` and `received/`. The installer's scratch folder is `ux0:data/pkg`. The format of `config.json` is listed in the
[user guide](USER_GUIDE.md#files-on-the-memory-card-ux0datavitasend). `config.json` is written to a temporary file and renamed over the old one, so a power loss cannot leave half a file.

## Conventions

* **Portable C (gnu11).** No C++ and no dynamic libraries. `-Wall -Wextra` must stay clean in the host build.
* **Small modules with a header that explains them.** Each `.c` starts with a comment saying what it is; each `.h` documents
  its functions and thread-safety.
* **Names.** `ls_` for utilities (`ls_log`, `ls_strlcpy`), module prefixes for the rest (`rx_`, `send_`, `xfer_`, `vpk_`,
  `gfx_`, `ui_`, `plat_`). Screens are `SC_*`, buttons `UI_BTN_*` (portable) / `PB_*` (the symbols on screen).
* **Memory.** Big buffers are `malloc`ed once (the picture, the stream buffers); the heap is raised to 64 MB on the Vita
  (`_newlib_heap_size_user` in `main.c`). Streaming code never holds a whole file.
* **Errors are shown, not hidden.** Failures end up in the log (`ls_log`, visible in *Settings > Log* and in `log.txt`) and
  in plain words on the transfer / install screen. Vita error codes are printed in hex.
* **Comments say *why*.** The Vita's behaviour is surprising in places (see the comments in `plat_vita.c` and `ui_vita.c`);
  keep that knowledge next to the code.
* **Time arithmetic on `uint64_t` stamps** is always guarded (a newer stamp minus an older one) - an unsigned wrap once made
  the screensaver restart itself right after waking up.

## Where to start for common changes

| I want to... | Look at |
|---|---|
| add a setting | `settings.h/.c` (field + JSON key), `app_settings.c` (row in the `ST_*` enum, text, change handler), `docs/USER_GUIDE.md` |
| add a page / dialog | `app_int.h` (`screen_id`, function list), a new or existing `app_*.c`, the dispatch in `app.c` (`draw_screen`, `handle_input`) |
| change colours or sizes | `ui.c` (themes, `CONTENT_*` layout constants) |
| support a new LocalSend field | `json` parsing in `rx.c` / `send.c` / `api.c`, then [PROTOCOL.md](PROTOCOL.md) |
| use another Vita function | declare in `plat.h`, implement in `plat_vita.c` **and** `plat_posix.c`, add the stub library to `LS_STUBS` in `CMakeLists.txt` |
| change the icon / LiveArea | `tools/gen_assets.py`, then rebuild the VPK |
| add a test | `host/test_transfer.py` (see [TESTING.md](TESTING.md)) |
