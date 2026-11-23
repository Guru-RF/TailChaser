# SPDX-License-Identifier: GPL-3.0-or-later — TailChaser, see LICENSE and AUTHORS
# TailChaser — one source tree, five programs:
#
#   tailchaser-simplex      simplex FM node on a reflector talkgroup
#   tailchaser-duplex       repeater, receiver and transmitter in one box
#   tailchaser-receiver     receive site of a split repeater
#   tailchaser-transmitter  transmit site of a split repeater
#   tailchaser-sdrreceiver  a receiver on a web SDR, to vote with
#
#   make                 release build of all five (on the Pi)
#   make debug           ASan/UBSan, -Og -g
#   make NOGPIO=1        build without libgpiod (desktop; file audio only)
#   make tools           vadtest, gatetest, dtmftest, agctest, ctcsstest (the detector, the gate,
#                        the DTMF decoder and the normaliser on WAV files)
#   make check           everything that needs no hardware: the migration script,
#                        the DTMF decoder and the gate on generated audio, the
#                        normaliser, then the programs end to end against
#                        stand-ins (needs python3 and the openssl command)
#   make interop REFLECTOR_IMAGE=<docker image>
#                        two nodes through a real svxreflector
#   make cutover         bootstrap.sh replace-svxlink / rollback and the service,
#                        rehearsed under systemd in a container (Docker, ~4 min)
#   sudo make install [ROLE=simplex]     one role, or all five without ROLE
#
# Target: Raspberry Pi OS / Debian aarch64, Bookworm or Trixie. The GPIO layer
# exists twice — src/gpio.c for libgpiod 2.x (Trixie), src/gpio_v1.c for 1.x
# (Bookworm) — and the right one is picked from the installed library.

CC      ?= cc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wno-unused-parameter \
           -Wno-overlength-strings
OPT     ?= -O2
PKGS    := alsa opus openssl
ifndef NOGPIO
PKGS    += libgpiod
endif

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs   $(PKGS) 2>/dev/null)

VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || cat VERSION 2>/dev/null || echo dev)
CFLAGS  := $(CSTD) $(WARN) $(OPT) -D_GNU_SOURCE $(PKG_CFLAGS) -DTAILCHASER_VERSION='"$(VERSION)"'
LDLIBS  := $(PKG_LIBS) -lpthread -lm -lresolv

ROLES := simplex duplex receiver transmitter sdrreceiver
BINS  := $(addprefix tailchaser-,$(ROLES))

SRC := src/main.c src/role.c src/config.c src/engine.c src/audio.c src/audioproc.c src/ctcss.c src/cw.c \
       src/gate.c src/vad.c src/dtmf.c \
       src/reflector.c src/refmsg.c src/refcrypto.c src/tls.c src/pki.c src/srv.c \
       src/jitter.c src/opus_codec.c \
       src/nettrx.c src/netrx.c src/sdr.c src/monitor.c \
       src/status.c src/admin.c src/sa818.c src/serial_worker.c src/mixer.c \
       src/detect.c src/log.c src/util.c src/wav.c
GPIOD_MAJOR := $(firstword $(subst ., ,$(shell pkg-config --modversion libgpiod 2>/dev/null)))
ifdef NOGPIO
SRC += src/gpio_stub.c
else ifeq ($(GPIOD_MAJOR),1)
SRC += src/gpio_v1.c
else
SRC += src/gpio.c
endif
OBJ := $(SRC:.c=.o)
DEP := $(OBJ:.o=.d) $(patsubst %,src/bin/%.d,$(ROLES))

TOOL_CFLAGS := $(CSTD) $(WARN) $(OPT) -D_GNU_SOURCE -Isrc
TOOLS := vadtest gatetest dtmftest agctest ctcsstest opuspkt iqgen

PREFIX  ?= /usr/local
DESTDIR ?=
ROLE    ?=
INSTALL_ROLES := $(if $(ROLE),$(ROLE),$(ROLES))

.PHONY: all debug clean install uninstall pkgcheck tools check interop cutover

all: pkgcheck $(BINS)

debug: OPT := -Og -g -fsanitize=address,undefined
debug: clean $(BINS)

# The five programs are the same objects behind five one-line main()s.
tailchaser-%: src/bin/%.o $(OBJ)
	$(CC) $(CFLAGS) $< $(OBJ) -o $@ $(LDLIBS)

# keep the objects when only one program is built (make tailchaser-receiver)
.SECONDARY: $(OBJ) $(patsubst %,src/bin/%.o,$(ROLES))

src/%.o: src/%.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# The admin page is a file of its own; it is compiled in as an array.
src/admin_page.h: src/admin.html
	{ echo 'static const unsigned char ADMIN_PAGE[] = {'; \
	  od -An -v -tx1 $< | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'; echo '};'; } > $@
src/admin.o: src/admin_page.h

-include $(DEP)

tools: $(TOOLS)

vadtest: tools/vadtest.c src/vad.c src/wav.c
	$(CC) $(TOOL_CFLAGS) $^ -o $@ -lm

dtmftest: tools/dtmftest.c src/dtmf.c src/wav.c
	$(CC) $(TOOL_CFLAGS) $^ -o $@ -lm

gatetest: tools/gatetest.c src/gate.c src/vad.c src/dtmf.c src/wav.c
	$(CC) $(TOOL_CFLAGS) $^ -o $@ -lm

agctest: tools/agctest.c src/audioproc.c src/wav.c
	$(CC) $(TOOL_CFLAGS) $^ -o $@ -lm

iqgen: tools/iqgen.c
	$(CC) $(CFLAGS) -o $@ $< -lm

opuspkt: tools/opuspkt.c
	$(CC) $(TOOL_CFLAGS) $(PKG_CFLAGS) $^ -o $@ -lopus

ctcsstest: tools/ctcsstest.c src/ctcss.c src/audioproc.c src/wav.c
	$(CC) $(TOOL_CFLAGS) $^ -o $@ -lm

# Without hardware: the programs play generated WAV files as squelch openings
# and record what they would transmit, while stand-ins take the part of the
# reflector, the repeater and the SA818.
check: $(BINS) dtmftest gatetest agctest ctcsstest opuspkt iqgen
	sh tests/migrate.sh ./tailchaser-simplex
	sh tests/soundcard.sh
	python3 tests/dtmf.py ./dtmftest ./gatetest
	python3 tests/agc.py ./agctest
	python3 tests/ctcss.py ./ctcsstest ./tailchaser-receiver
	python3 tests/announce.py ./tailchaser-duplex tools/tailchaser-announce
	python3 tools/devcal.py --selftest
	python3 tools/sdrlag.py --selftest
	python3 tests/admin.py ./tailchaser-duplex
	python3 tests/monitor.py ./tailchaser-duplex
	python3 tests/sdr.py ./tailchaser-sdrreceiver ./opuspkt
	python3 tests/usbsdr.py ./tailchaser-receiver ./iqgen
	python3 tests/voter.py ./tailchaser-receiver ./tailchaser-duplex ./tailchaser-simplex
	python3 tests/multirx.py ./tailchaser-receiver ./tailchaser-duplex ./tailchaser-transmitter ./tailchaser-sdrreceiver
	python3 tests/e2e_receiver.py ./tailchaser-receiver
	python3 tests/e2e_repeater.py ./tailchaser-receiver ./tailchaser-transmitter ./tailchaser-duplex
	python3 tests/e2e_simplex.py ./tailchaser-simplex

# Not part of `make check`: these need Docker.
interop: tailchaser-simplex
	python3 tests/interop.py ./tailchaser-simplex $(REFLECTOR_IMAGE)

cutover:
	sh tests/cutover.sh

pkgcheck:
	@pkg-config --exists $(PKGS) || { \
	  echo "ERROR: missing dev packages. Install: libasound2-dev libopus-dev libgpiod-dev libssl-dev"; \
	  echo "       (no libgpiod on this machine? build with: make NOGPIO=1)"; \
	  exit 1; }
	@echo "libgpiod: $$(pkg-config --modversion libgpiod 2>/dev/null || echo none) -> $(filter src/gpio%,$(SRC))"

clean:
	$(RM) src/*.o src/*.d src/bin/*.o src/bin/*.d src/admin_page.h $(BINS) $(TOOLS)

install: $(addprefix tailchaser-,$(INSTALL_ROLES))
	@for r in $(INSTALL_ROLES); do \
	  install -Dm755 tailchaser-$$r $(DESTDIR)$(PREFIX)/sbin/tailchaser-$$r && \
	  install -Dm644 systemd/tailchaser-$$r.service $(DESTDIR)/etc/systemd/system/tailchaser-$$r.service && \
	  install -Dm644 config/$$r.conf.example $(DESTDIR)/etc/tailchaser/$$r.conf.example && \
	  echo "installed tailchaser-$$r"; \
	done
	@install -Dm755 tools/tailchaser-announce $(DESTDIR)$(PREFIX)/bin/tailchaser-announce
	@install -Dm755 tools/tailchaser-soundcard $(DESTDIR)$(PREFIX)/sbin/tailchaser-soundcard
	@install -Dm644 systemd/tailchaser-soundcard.service $(DESTDIR)/etc/systemd/system/tailchaser-soundcard.service
	@install -Dm644 systemd/tailchaser-announce@.service $(DESTDIR)/etc/systemd/system/tailchaser-announce@.service
	@install -Dm644 systemd/tailchaser-announce@.timer $(DESTDIR)/etc/systemd/system/tailchaser-announce@.timer

uninstall:
	@for r in $(INSTALL_ROLES); do \
	  $(RM) $(DESTDIR)$(PREFIX)/sbin/tailchaser-$$r $(DESTDIR)/etc/systemd/system/tailchaser-$$r.service; \
	done
