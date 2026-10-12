CXX ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -ffast-math -pthread
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share

# Check for libraries via pkg-config, or fallback to standard flags
PKG_CONFIG ?= pkg-config
PW_LIBS := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null || echo "-lpipewire-0.3")
PW_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null || echo "-I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2")

PULSE_LIBS := $(shell $(PKG_CONFIG) --libs libpulse libpulse-simple 2>/dev/null || echo "-lpulse -lpulse-simple")
PULSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpulse libpulse-simple 2>/dev/null || echo "")

ALSA_LIBS := $(shell $(PKG_CONFIG) --libs alsa 2>/dev/null || echo "-lasound")
ALSA_CFLAGS := $(shell $(PKG_CONFIG) --cflags alsa 2>/dev/null || echo "")

GTK_LIBS := $(shell $(PKG_CONFIG) --libs gtk+-3.0 ayatana-appindicator3-0.1 2>/dev/null || echo "-lgtk-3 -layatana-appindicator3")
GTK_CFLAGS := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 ayatana-appindicator3-0.1 2>/dev/null || echo "-I/usr/include/gtk-3.0")

ALL_CFLAGS := $(CXXFLAGS) $(PW_CFLAGS) $(PULSE_CFLAGS) $(ALSA_CFLAGS) $(GTK_CFLAGS) -MMD -MP
ALL_LIBS := $(PW_LIBS) $(PULSE_LIBS) $(ALSA_LIBS) $(GTK_LIBS) -pthread

SRCS = src/main.cpp \
       src/dsp.cpp \
       src/config.cpp \
       src/ipc.cpp \
       src/gui.cpp \
       src/audio_backend.cpp \
       src/pipewire_backend.cpp \
       src/pulse_backend.cpp \
       src/alsa_backend.cpp \
       src/benchmark.cpp

OBJS = $(SRCS:.cpp=.o)
TARGET = bin/crossfeed

.PHONY: all clean install uninstall bench deb rpm xbps pkg check-deps test update upgrade check-update

all: $(TARGET)

check-deps:
	@if ! $(PKG_CONFIG) --exists gtk+-3.0 2>/dev/null; then \
		echo "=================================================================" >&2; \
		echo "ERROR: GTK 3 development headers (gtk/gtk.h) not found!" >&2; \
		echo "" >&2; \
		echo "KDE Plasma and minimal desktop systems do not include GTK headers by default." >&2; \
		echo "Install the development packages for your distribution:" >&2; \
		echo "" >&2; \
		echo "  Fedora / RHEL:        sudo dnf install gtk3-devel libayatana-appindicator-gtk3-devel" >&2; \
		echo "  Debian / Ubuntu:      sudo apt install libgtk-3-dev libayatana-appindicator3-dev" >&2; \
		echo "  Arch Linux / Manjaro: sudo pacman -S gtk3 libayatana-appindicator" >&2; \
		echo "  openSUSE:             sudo zypper install gtk3-devel libayatana-appindicator3-devel" >&2; \
		echo "  Void Linux:           sudo xbps-install gtk+3-devel libayatana-appindicator-devel" >&2; \
		echo "" >&2; \
		echo "TIP: To avoid compiling, install the pre-built packages in dist/ directly:" >&2; \
		echo "  Fedora / rpm-ostree:  sudo dnf install dist/*.rpm" >&2; \
		echo "  Ubuntu / Debian:      sudo apt install dist/*.deb" >&2; \
		echo "  Void Linux:           sudo xbps-install -R dist crossfeed" >&2; \
		echo "=================================================================" >&2; \
		exit 1; \
	fi

$(TARGET): $(OBJS) | bin
	$(CXX) $(OBJS) $(ALL_LIBS) -o $@

$(OBJS): | check-deps

%.o: %.cpp
	$(CXX) $(ALL_CFLAGS) -c $< -o $@

bin:
	mkdir -p bin

TEST_TARGET = bin/test_dsp
TEST_SRCS = test/test_dsp.cpp src/dsp.cpp src/config.cpp

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRCS) | bin check-deps
	$(CXX) $(CXXFLAGS) -Isrc $(TEST_SRCS) -o $@

bench: $(TARGET)
	./$(TARGET) bench

deb: $(TARGET)
	bash ./scripts/package.sh deb

rpm: $(TARGET)
	bash ./scripts/package.sh rpm

xbps: $(TARGET)
	bash ./scripts/package.sh xbps

pkg: $(TARGET)
	bash ./scripts/package.sh all

check-update:
	@bash ./scripts/update.sh --check

update upgrade:
	@bash ./scripts/update.sh

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET) $(TEST_TARGET)
	rm -rf bin dist build

-include $(OBJS:.o=.d)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/crossfeed
	install -d $(DESTDIR)$(DATADIR)/applications
	install -m 644 data/crossfeed.desktop $(DESTDIR)$(DATADIR)/applications/crossfeed.desktop
	install -d $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps
	install -m 644 data/crossfeed.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/crossfeed.svg
	install -d $(DESTDIR)$(DATADIR)/licenses/crossfeed
	install -m 644 LICENSE $(DESTDIR)$(DATADIR)/licenses/crossfeed/LICENSE

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/crossfeed
	rm -f $(DESTDIR)$(DATADIR)/applications/crossfeed.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/crossfeed.svg
	rm -rf $(DESTDIR)$(DATADIR)/licenses/crossfeed
