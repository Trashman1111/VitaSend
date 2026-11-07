# VitaSend - author: trashman
cmake_minimum_required(VERSION 3.5)

# ---- VitaSDK toolchain (must be chosen before project()) ------------------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "VITASDK is not set.  Example:  export VITASDK=/usr/local/vitasdk")
  endif()
endif()

project(VitaSend C)
include("$ENV{VITASDK}/share/vita.cmake")

message(STATUS "VitaSend v0.9.7 (LiveArea version 00.97)")

# ---- options ----------------------------------------------------------------------------------------
set(VITA_TITLEID  "LSND00001")          # 9 characters: 4 letters + 5 digits
set(VITA_VERSION  "00.97")              # shown in LiveArea / VitaShell (digits only: 00.97 = v0.9.7)
set(VITA_APP_NAME "VitaSend")

option(LS_UNSAFE "Build the app with extended permissions (\"unsafe homebrew\") - needed by the .vpk installer" ON)
option(HELLO_ONLY "Phase-1 build: hello world + mbedTLS self-tests only (no network)" OFF)
set(MBEDTLS_ROOT "${CMAKE_SOURCE_DIR}/deps/mbedtls-vita" CACHE PATH "Where scripts/build-mbedtls.sh installed mbedTLS")

if(NOT EXISTS "${MBEDTLS_ROOT}/lib/libmbedtls.a")
  message(FATAL_ERROR
    "mbedTLS not found in ${MBEDTLS_ROOT}\n"
    "Run  ./scripts/build-mbedtls.sh  first (or pass -DMBEDTLS_ROOT=/path/to/install).")
endif()

# ---- compiler flags ---------------------------------------------------------------------------------
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu11")
if(HELLO_ONLY)
  add_definitions(-DLS_HELLO_ONLY)
endif()

add_definitions(-DLS_TITLE_ID="${VITA_TITLEID}")
include_directories(${CMAKE_SOURCE_DIR}/src ${MBEDTLS_ROOT}/include)
link_directories(${MBEDTLS_ROOT}/lib)

# ---- sources ----------------------------------------------------------------------------------------
set(LS_SOURCES
  src/main.c
  src/util.c
  src/json.c
  src/tls.c
  src/identity.c
  src/settings.c
  src/history.c
  src/xfer.c
  src/peers.c
  src/http.c
  src/api.c
  src/rx.c
  src/send.c
  src/discovery.c
  src/gfx.c
  src/gfx_icons.c
  src/fontdata.c
  src/ui.c
  src/touch.c
  src/inflate.c
  src/zip.c
  src/headbin.c
  src/vpk.c
  src/unzip.c
  src/app.c
  src/app_files.c
  src/app_pages.c
  src/app_settings.c
  src/app_dialogs.c
  src/app_install.c
  src/app_move.c
  src/app_fileops.c
  src/app_view.c
  src/app_update.c
  src/update.c
  src/cacerts.c
  src/wifi.c
  src/app_saver.c
  src/plat_vita.c
  src/ui_vita.c
)
add_executable(${PROJECT_NAME} ${LS_SOURCES})

# ---- libraries --------------------------------------------------------------------------------------
# Stub libraries every VitaSDK has.
set(LS_STUBS
  SceNet_stub SceNetCtl_stub SceSysmodule_stub SceCtrl_stub SceDisplay_stub
  ScePower_stub SceLibKernel_stub SceTouch_stub SceAudio_stub)

# The .vpk installer needs the system's ScePromoterUtil. Every current VitaSDK ships its stub library; if yours does not,
# the app still builds (the installer then reports "no installer support").
if(EXISTS "$ENV{VITASDK}/arm-vita-eabi/lib/libScePromoterUtil_stub.a")
  list(APPEND LS_STUBS ScePromoterUtil_stub)
else()
  message(WARNING "libScePromoterUtil_stub.a not found in your VitaSDK - building WITHOUT the .vpk installer (update VitaSDK to get it).")
  add_definitions(-DLS_NO_PROMOTER)
endif()

# Stub libraries whose functions live in SceLibKernel on some VitaSDK versions and in their own
# library on others - link them only if this SDK has them, so the build works either way.
foreach(stub SceProcessmgr_stub SceThreadmgr_stub SceSysmem_stub SceIofilemgr_stub SceRng_stub SceLibRng_stub)
  if(EXISTS "$ENV{VITASDK}/arm-vita-eabi/lib/lib${stub}.a")
    list(APPEND LS_STUBS ${stub})
  endif()
endforeach()

# The wall-clock time (screensaver clock) comes from SceRtc when this SDK has its stub library, otherwise from time() (UTC).
if(EXISTS "$ENV{VITASDK}/arm-vita-eabi/lib/libSceRtc_stub.a")
  list(APPEND LS_STUBS SceRtc_stub)
  add_definitions(-DLS_HAVE_RTC)
endif()

# Opening an installed app (Square on the "Installed" screen) uses SceAppMgr; without its stub library that button is simply not offered.
if(EXISTS "$ENV{VITASDK}/arm-vita-eabi/lib/libSceAppMgr_stub.a")
  list(APPEND LS_STUBS SceAppMgr_stub)
  add_definitions(-DLS_HAVE_APPMGR)
endif()

# "Turn on Wi-Fi" inside the app uses SceWlanBt's sceWlanSetConfiguration (linked weakly). Without that stub library the button
# still appears, but answers "not possible here" and points to the Vita settings.
if(EXISTS "$ENV{VITASDK}/arm-vita-eabi/lib/libSceWlanBt_stub_weak.a")
  list(APPEND LS_STUBS SceWlanBt_stub_weak)
  add_definitions(-DLS_HAVE_WLAN)
endif()

target_link_libraries(${PROJECT_NAME}
  mbedtls mbedx509 mbedcrypto
  ${LS_STUBS}
  m)

# ---- SELF + VPK -------------------------------------------------------------------------------------
if(LS_UNSAFE)
  # "Unsafe" = extended permissions. Without them the system refuses to load the internal modules the installer uses.
  # The app then needs "Enable unsafe homebrew" in the HENkaku settings, like VitaShell.
  vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
else()
  vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
endif()

vita_create_vpk(${PROJECT_NAME}-v097.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME    ${VITA_APP_NAME}
  FILE sce_sys/icon0.png                          sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg0.png          sce_sys/livearea/contents/bg0.png
  FILE sce_sys/livearea/contents/startup.png      sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml     sce_sys/livearea/contents/template.xml
)
