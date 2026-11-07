# Building VitaSend

You do not need to build anything to *use* VitaSend - download the `.vpk` from the
[Releases](../../../releases) page. This page is for building it yourself.

## What you need

* **[VitaSDK](https://vitasdk.org/)** (`vdpm`) with `VITASDK` pointing at it - Linux, macOS, Windows through **WSL**, or the
  official Docker image.
* CMake 3.5 or newer, Python 3, curl, bzip2 and a normal C compiler for the host (mbedTLS' own build scripts use them).
* About 400 MB of disk space for the mbedTLS build.

## Build

```sh
export VITASDK=/usr/local/vitasdk            # wherever yours is
export PATH=$VITASDK/bin:$PATH

./scripts/build-mbedtls.sh                   # once: downloads mbedTLS 3.6.2 (SHA-256 checked) and builds it into deps/mbedtls-vita
mkdir build && cd build
cmake ..
make -j4
```

Result: `build/VitaSend-v<version>.vpk` (for example `VitaSend-v096.vpk`). Install it with VitaShell (copy it to the Vita
by USB or FTP, open it, press X) and enable **"Enable unsafe homebrew"** in the HENkaku settings - see
[why](#why-the-app-is-unsafe).

`build-mbedtls.sh` never touches your VitaSDK installation, so other Vita projects keep working. It switches off the mbedTLS
modules the Vita cannot use (net_sockets, timing, platform entropy) and plugs in the Vita's hardware random number
generator (`mbedtls_hardware_poll` in `src/tls.c`).

### With Docker (no local SDK)

```sh
docker run --rm -v "$PWD":/src -w /src vitasdk/vitasdk bash -c \
  "./scripts/build-mbedtls.sh && mkdir -p build && cd build && cmake .. && make -j4"
```

The GitHub Actions workflows in `.github/workflows/` do exactly this.

### Windows

Use WSL (Ubuntu), install VitaSDK there as described on vitasdk.org, and build **inside the Linux file system**
(`~/vitasend`), not under `/mnt/c/...` - it is much faster and avoids permission problems. Copy the finished `.vpk` out with
`cp build/*.vpk /mnt/c/Users/<you>/Downloads/` or open the folder with `explorer.exe .`.

## CMake options

| Option | Default | Meaning |
|---|---|---|
| `-DLS_UNSAFE=ON/OFF` | `ON` | Build the SELF with extended permissions. Needed for the installer (see below). `OFF` builds an app that works everywhere but cannot install `.vpk` files. |
| `-DHELLO_ONLY=ON` | `OFF` | A tiny first-boot build: shows the device identity and runs the self tests (random numbers, SHA-256, certificate, an in-memory TLS handshake), no network. Good for checking a new toolchain. |
| `-DMBEDTLS_ROOT=/path` | `deps/mbedtls-vita` | Use an mbedTLS you built yourself (it must be configured like `build-mbedtls.sh` does). |

In `CMakeLists.txt` you can also change `VITA_TITLEID` (default `LSND00001`; 4 letters + 5 digits, the id of the installed
bubble), `VITA_VERSION` (the digits shown in the LiveArea) and the app name. CMake detects optional stub libraries in your
SDK (`ScePromoterUtil`, `SceRtc`, `SceAppMgr`) and compiles the matching features in or out - the app builds either way.

## Why the app is "unsafe"

The `.vpk` installer hands the unpacked package to the system's `ScePromoterUtil`, a module only applications with extended
permissions may load - exactly like VitaShell. So the VPK is created with `vita_create_self(... UNSAFE)`, and the Vita needs
*Enable unsafe homebrew* (HENkaku settings) or it will refuse to start the app. If you do not want that, build with
`-DLS_UNSAFE=OFF`; everything else works, and the install step reports "Cannot load the system module PAF".

## The PC test build

```sh
./scripts/build-mbedtls.sh host            # mbedTLS for your PC, into deps/mbedtls-host
make -C host SAN=1                          # host/localsend-host (with AddressSanitizer + UBSan)
```

This compiles the whole program (except the two Vita files) as an ordinary executable. See [TESTING.md](TESTING.md).

## Check a change against the real Vita headers without a VitaSDK build

`plat_vita.c` and `ui_vita.c` are the only files that use Vita functions. If you only have the headers
([vita-headers](https://github.com/vitasdk/vita-headers)) you can at least check that they compile cleanly:

```sh
gcc -fsyntax-only -std=gnu11 -Wall -Wextra -Wno-unused-parameter -D__vita__ -DLS_HAVE_RTC -DLS_HAVE_APPMGR \
    -DLS_TITLE_ID='"LSND00001"' -Isrc -I/path/to/vita-headers/include -Ideps/mbedtls-host/include src/plat_vita.c
```

## Build problems

* **`mbedTLS not found`** - run `./scripts/build-mbedtls.sh` first (needs `VITASDK`).
* **`cannot find -lSceXxx_stub`** or **undefined reference to `sceXxx`** - your VitaSDK puts that function in a different
  stub library. Edit `LS_STUBS` in `CMakeLists.txt` (`ls $VITASDK/arm-vita-eabi/lib/libSce*_stub.a` shows what you have).
* **A `SCE_NET_...` constant or type is unknown** - those are used in `src/plat_vita.c`; compare with
  `$VITASDK/arm-vita-eabi/include/psp2/net/net.h`.
* **`libScePromoterUtil_stub.a not found`** (CMake warning) - update VitaSDK (`vdpm`); until then the app is built without
  installer support.
* **The build folder is from an older version** - delete it: `rm -rf build`.
* **App crashes right at start on the Vita** - build with `-DHELLO_ONLY=ON` to see whether it is the TLS self test or the
  network part. The framebuffer setup is in `ui_init()` in `src/ui_vita.c`.

## Releasing a new version

1. Bump the version in three places: `LS_APP_VERSION` and `LS_APP_DATE` in `src/util.h`, `VITA_VERSION` / the `.vpk` name /
   the `message()` line in `CMakeLists.txt`, and `VERSION` in `host/test_transfer.py` (the test checks the About box).
2. Add a section to [CHANGELOG.md](../CHANGELOG.md).
3. Run the tests ([TESTING.md](TESTING.md)).
4. Tag it: `git tag v0.9.6 && git push --tags`. The release workflow builds the VPK and attaches it to a GitHub release.
