#!/usr/bin/env bash
#
# Builds mbedTLS 3.6 LTS as a static library, into  deps/mbedtls-vita  (or deps/mbedtls-host).
#
#   ./scripts/build-mbedtls.sh          # for the PS Vita   (needs VitaSDK:  export VITASDK=/usr/local/vitasdk)
#   ./scripts/build-mbedtls.sh host     # for your PC, only needed for the optional PC test build in host/
#
# It does NOT touch your VitaSDK install, so other Vita projects that use the vdpm mbedtls keep working.
set -euo pipefail

MODE="${1:-vita}"
VER="3.6.2"
SHA256="8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca"
URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${VER}/mbedtls-${VER}.tar.bz2"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${ROOT}/deps"
TARBALL="${DEPS}/mbedtls-${VER}.tar.bz2"
SRC="${DEPS}/src-${MODE}/mbedtls-${VER}"
BUILD="${DEPS}/build-${MODE}"
PREFIX="${DEPS}/mbedtls-${MODE}"

case "$MODE" in
  vita)
    : "${VITASDK:?VITASDK is not set. Example: export VITASDK=/usr/local/vitasdk}"
    TOOLCHAIN="${VITASDK}/share/vita.toolchain.cmake"
    [ -f "$TOOLCHAIN" ] || { echo "Cannot find $TOOLCHAIN - is VitaSDK installed?"; exit 1; }
    CMAKE_EXTRA=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN")
    ;;
  host)
    CMAKE_EXTRA=()
    ;;
  *)
    echo "usage: $0 [vita|host]"; exit 2 ;;
esac

mkdir -p "$DEPS"

echo "== 1/4 download mbedTLS ${VER}"
if [ ! -f "$TARBALL" ]; then
  curl -L --fail -o "$TARBALL" "$URL"
fi
echo "${SHA256}  ${TARBALL}" | sha256sum -c - || { echo "Checksum mismatch - delete $TARBALL and try again"; exit 1; }

echo "== 2/4 unpack + configure for ${MODE}"
rm -rf "${DEPS}/src-${MODE}" "$BUILD" "$PREFIX"
mkdir -p "${DEPS}/src-${MODE}"
tar -xjf "$TARBALL" -C "${DEPS}/src-${MODE}"

# The Vita has no /dev/urandom, no BSD-socket helper module and no POSIX timers, so:
#   - entropy comes from our own mbedtls_hardware_poll() (src/tls.c -> sceKernelGetRandomNumber)
#   - the net_sockets and timing modules are switched off (we use sceNet directly)
CFG="${SRC}/scripts/config.py"
python3 "$CFG" -f "${SRC}/include/mbedtls/mbedtls_config.h" set   MBEDTLS_NO_PLATFORM_ENTROPY
python3 "$CFG" -f "${SRC}/include/mbedtls/mbedtls_config.h" set   MBEDTLS_ENTROPY_HARDWARE_ALT
python3 "$CFG" -f "${SRC}/include/mbedtls/mbedtls_config.h" unset MBEDTLS_NET_C
python3 "$CFG" -f "${SRC}/include/mbedtls/mbedtls_config.h" unset MBEDTLS_TIMING_C
# No wall-clock / monotonic-clock dependency inside mbedTLS (we verify peers by fingerprint, not by date).
python3 "$CFG" -f "${SRC}/include/mbedtls/mbedtls_config.h" unset MBEDTLS_HAVE_TIME_DATE
python3 "$CFG" -f "${SRC}/include/mbedtls/mbedtls_config.h" unset MBEDTLS_HAVE_TIME
# Keep the build to the three classic libraries (no separate libeverest / libp256m).
python3 "$CFG" -f "${SRC}/include/mbedtls/mbedtls_config.h" unset MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED
python3 "$CFG" -f "${SRC}/include/mbedtls/mbedtls_config.h" unset MBEDTLS_PSA_P256M_DRIVER_ENABLED

echo "== 3/4 build"
cmake -S "$SRC" -B "$BUILD" "${CMAKE_EXTRA[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-O2" \
  -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
  -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
  -DMBEDTLS_FATAL_WARNINGS=OFF -DGEN_FILES=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)"

echo "== 4/4 install into ${PREFIX}"
cmake --install "$BUILD"

echo
echo "Done. Libraries:"
ls -1 "${PREFIX}/lib"/*.a
echo
if [ "$MODE" = "vita" ]; then
  echo "Next:  mkdir build && cd build && cmake .. && make"
else
  echo "Next:  make -C host"
fi
