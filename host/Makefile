# VitaSend - author: trashman
# Optional: build the same program for your PC (Linux/macOS) to test the network code without a Vita.
#
#   ./scripts/build-mbedtls.sh host     # once
#   make -C host
#   ./host/localsend-host --port 53320 --seconds 60     # shows up in the real LocalSend app on your LAN
#
# Options:  make -C host SAN=1   (AddressSanitizer + UBSan)     make -C host HELLO=1  (phase-1 only)
ROOT  := $(abspath ..)
MBED  ?= $(ROOT)/deps/mbedtls-host
SRC   := $(ROOT)/src
OUT   := $(ROOT)/host/localsend-host

SOURCES := main util json tls identity settings history xfer peers http api rx send discovery gfx gfx_icons fontdata ui app app_files app_pages app_settings app_dialogs app_install app_move app_fileops app_view app_update wifi update cacerts app_saver inflate zip headbin vpk unzip plat_posix touch ui_host

CC      ?= gcc
CFLAGS  := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -DLS_HOST -I$(SRC) -I$(MBED)/include
LDFLAGS := -L$(MBED)/lib -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -lm

ifdef SAN
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif
ifdef HELLO
CFLAGS  += -DLS_HELLO_ONLY
endif

$(OUT): $(addprefix $(SRC)/,$(addsuffix .c,$(SOURCES))) $(wildcard $(SRC)/*.h)
	$(CC) $(CFLAGS) $(addprefix $(SRC)/,$(addsuffix .c,$(SOURCES))) -o $@ $(LDFLAGS)

clean:
	rm -f $(OUT)

.PHONY: clean
