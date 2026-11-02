# wate - Wayland terminal emulator: make, make install, make clean

VERSION  = 0.1
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DESTDIR ?=

CC      ?= gcc
CFLAGS  ?= -O2 -g
PKG_CONFIG ?= pkg-config

LIBRARIES = wayland-client wayland-cursor xkbcommon freetype2 fontconfig

PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(LIBRARIES) 2>/dev/null)
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(LIBRARIES) 2>/dev/null)

WARN     = -Wall -Wextra
HARDEN   = -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fno-strict-overflow
ALL_CFLAGS = -std=gnu11 -ffunction-sections -fdata-sections -Ibuild/gen -DWATE_VERSION='"$(VERSION)"' $(HARDEN) \
             $(CFLAGS) $(WARN) $(PKG_CFLAGS) -Isrc -MMD -MP
LDLIBS  = $(PKG_LIBS) -lutil -lm
LDOPTS  = -Wl,--as-needed -Wl,--gc-sections -Wl,-O1

SRC_OWN = main config font boxdraw render wl mouse
SRC_TSM = tsm-render tsm-screen tsm-selection tsm-unicode tsm-vte tsm-vte-charsets \
          shl-htable wcwidth
SRC_PRO = xdg-shell xdg-decoration-unstable-v1 primary-selection-unstable-v1 \
          viewporter fractional-scale-v1

OBJ = $(addprefix build/,$(addsuffix .o,$(SRC_OWN))) \
      $(addprefix build/tsm/,$(addsuffix .o,$(SRC_TSM))) \
      $(addprefix build/protocol/,$(addsuffix .o,$(SRC_PRO)))

all: check-deps wate

check-deps:
	@$(PKG_CONFIG) --exists $(LIBRARIES) || { \
	  echo "error: missing development files; need (pkg-config names):"; \
	  echo "       $(LIBRARIES)"; \
	  for l in $(LIBRARIES); do $(PKG_CONFIG) --exists $$l || echo "       not found: $$l"; done; \
	  exit 1; }

wate: $(OBJ)
	$(CC) $(LDFLAGS) $(LDOPTS) -o $@ $(OBJ) $(LDLIBS)

build/gen/default_config.h: default/config.py
	@mkdir -p $(@D)
	( echo 'static const char wate_default_config[] ='; \
	  sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' default/config.py; \
	  echo ';' ) > $@

build/config.o: build/gen/default_config.h

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

build/tsm/%.o: WARN = -w
build/protocol/%.o: WARN = -w

clean:
	rm -rf build wate

install: wate
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 wate $(DESTDIR)$(BINDIR)/wate
	install -d $(DESTDIR)/etc/wate
	install -m 644 default/config.py $(DESTDIR)/etc/wate/config.py

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/wate $(DESTDIR)/etc/wate/config.py

-include $(OBJ:.o=.d)

.PHONY: all check-deps clean install uninstall
