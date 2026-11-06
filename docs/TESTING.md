# Testing

Almost the whole program is portable C, so it is tested on a PC without a Vita: the program is compiled as a normal
executable (`host/`) against the real mbedTLS, run under AddressSanitizer / UBSan, and driven by Python scripts that play
**the other LocalSend device** (HTTPS client *and* server) and **press the Vita's buttons**.

```sh
./scripts/build-mbedtls.sh host            # once
make -C host SAN=1                          # builds host/localsend-host
python3 host/fake_peer.py                   # protocol checks: discovery, /info, /register, malformed requests (39)
python3 host/fake_peer.py --chunked-replies
python3 host/test_transfer.py               # the big one: receive, send, PIN, install, touch, settings, UI ... (more than 750 checks, several minutes)
```

You need Python 3 and the `openssl` command line tool (the scripts make throw-away certificates). Pillow
(`pip install pillow`) is only needed for the screenshot mode.

Run a part of it with `--only`:

```sh
python3 host/test_transfer.py --only install    # installer + Files tab (builds real .vpk files)
python3 host/test_transfer.py --only touch      # simulated fingers
python3 host/test_transfer.py --only queue      # install queue, automatic installation, chimes
python3 host/test_transfer.py --only move       # copying and moving files: rename, copy between cards, cancel, conflicts
python3 host/test_transfer.py --only view       # the picture / text viewer, file queues, the Files keys, the slim menu
python3 host/test_transfer.py --only update     # update check / download / self-install against a fake HTTPS GitHub with its own certificate authority (needs the openssl command)
python3 host/test_transfer.py --only fileops    # delete with progress, unzip (progress, safety, damaged files, cancel)
python3 host/test_transfer.py --only wifi       # Wi-Fi off at the start / lost while running: the dialog, the button, cancel, refusal, continuing offline
```

(`recv send extra install privacy theme touch migrate files exists retry queue polish dpad x11 x13 move wifi view update fileops` - run
`python3 host/test_transfer.py --help` for the list.) Add `--keep` to keep the temporary folder with the logs.

## What the tests cover

Accept / reject, chunked and plain uploads, SHA-256 mismatch, path traversal names, duplicate names, dropped connections,
cancel from both sides, 25 MB files, 4 parallel uploads, 120 files in one request, PIN and the lock-out, favourites,
Quick Save, text messages both ways, folders, partial acceptance, a receiver that disappears, retry; the `.vpk` installer
(ZIP / DEFLATE compared with Python's `zlib` on many inputs, truncated and corrupted streams, malicious packages, queue,
automatic installation, cancel); settings and themes; the touch screen (every kind of tap and drag); the D-pad focus rules;
the screensaver wake rules; the chimes.

## How the harness works

`host/localsend-host` takes a few extra options:

| Option | Meaning |
|---|---|
| `--data DIR` | use this folder instead of `ux0:data/VitaSend` |
| `--port N` / `--announce-to IP:PORT` | listen on this port / send the multicast announcement to a test peer instead |
| `--seconds N` | quit by itself after N seconds |
| `--dump-screen FILE` | write the **text of the screen** whenever it changes: a first line `@ tab=... screen=...`, one line per UI row, `>` in column 0 marks the focused row |
| `--dump-ppm FILE` | write the **picture** (960 x 544 PPM), at most every 250 ms |
| `--keys "cross,down,wait500"` | button presses at start-up |
| `--keys-fifo PATH` | a named pipe: write more key names into it while the program runs |
| `--browse-start DIR`, `--free-space BYTES` | where the file browser starts / pretend the card has only this much room |

Key names: `up down left right cross circle square triangle start select l r`, `waitNNN` (ms). Touch:
`tap:X:Y` and `drag:X1:Y1:X2:Y2`. `FILE.hits` lists the touch areas of the current screen.

Environment variables (test knobs, only in the PC build):

| Variable | Effect |
|---|---|
| `LS_KEY_MS=60` | faster simulated button presses (default 150 ms) |
| `LS_FAKE_BATTERY=69`, `LS_FAKE_CHARGING=1` | pretend battery percentage / charging |
| `LS_FAKE_CLOCK=14:07` | pretend wall-clock time |
| `LS_IDLE_UNIT_MS=100` | shrinks "seconds" of the screensaver timer (30 s -> 3 s) |
| `LS_SLOW_PROMOTE_MS`, `LS_FAKE_PROMOTE_FAIL` | the stand-in installer takes long / fails |
| `LS_FAIL_RENAME=1`, `LS_SLOW_MOVE_MS=60` | renaming always fails (so *Move* copies, like between two cards) / each copied 64 KB block takes this long |
| `LS_UPDATE_API=localhost:PORT`, `LS_UPDATE_CA=ca.pem`, `LS_UPDATE_DELAY_MS`, `LS_FAKE_DATE=YYYY-MM-DD` | the update check asks this server instead of api.github.com / trusts only this CA instead of the built-in roots / waits this long after the IP appears (default 4000) / the date for the once-a-day rule |
| `LS_SLOW_DELETE_MS=40`, `LS_SLOW_UNZIP_MS=60` | each deleted entry / each unpacked block takes this long (so a test can press Stop / Cancel) |
| `LS_WIFI_GRACE_MS=4000` | wait this long for Wi-Fi at start before the *turned off* window (the Vita build always waits 5 s) |
| `LS_WIFI_FILE=path`, `LS_WIFI_TURNON=ok\|fail\|nojoin`, `LS_WIFI_DELAY_MS`, `LS_WIFI_TIMEOUT_MS`, `LS_WIFI_SETTINGS_FAIL=1` (opening the Wi-Fi settings fails; the log shows which address would be opened) | a fake Wi-Fi switch: the file says `off` while Wi-Fi is off (the test writes it); "switch on" works after the delay / is refused / is accepted but never connects; how long the app waits for the network |
| `LS_SLOW_KEEPAWAKE_MS`, `LS_SLOW_INPUT_MS` | slow calls, to shake out timing bugs (they found real ones on a Vita) |

`test_transfer.py` is organised as one function per area (`test_receive()`, `test_install()`, ...), with a `Vita` class that
starts the program and offers `keys()`, `tap()`, `has("text")`, `wait_screen("text")`, `config()`, `log()`. Add a test by
copying the pattern of a small existing one (`test_x13()` is short).

## Screenshots

```sh
python3 host/test_transfer.py --only shots --shots-dir docs/screens
```

renders the pictures in `docs/screens/` with the very same drawing code the Vita runs (battery 69 %, made-up device names,
documentation addresses). Regenerate them after a visual change.

## What the tests cannot tell you

The two files that call the Vita (`plat_vita.c`, `ui_vita.c`) are only compiled against the real SDK headers, not run by the
tests. Anything that depends on the console - the system installer, real Wi-Fi behaviour, touch panel scaling, power state,
speed - needs a real Vita. When you change those files, say in your pull request how you tested on hardware.
