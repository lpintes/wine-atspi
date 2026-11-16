# Build with winegcc in Arch WSL: make
# Run in the test prefix:         make run-probe

WINEGCC ?= winegcc
CFLAGS ?= -O2 -g -Wall
WINEPREFIX ?= $(HOME)/.wine-atspi
BUILD = build
# Win32 modules use Wine's msvcrt headers, not glibc.
WIN32_FLAGS = -mno-cygwin -mconsole

export WINEPREFIX

all: $(BUILD)/probe.exe.so $(BUILD)/sendkeys.exe.so $(BUILD)/controls.exe.so \
	$(BUILD)/wine-atspi.exe.so $(BUILD)/wine-atspi-hook.dll

$(BUILD):
	mkdir -p $@

# AT-SPI side: host gcc against glibc and GLib, no Windows headers.
CC ?= gcc
GIO_CFLAGS := $(shell pkg-config --cflags gio-2.0)
GIO_LIBS := $(shell pkg-config --libs gio-2.0)

$(BUILD)/atspi.o: bridge/atspi.c bridge/bridge.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC $(GIO_CFLAGS) -c -o $@ $<

# Win32 core: winegcc, Windows headers only (see bridge/core.h).
CORE = main util object range text labels listview tab toolbar listbox treeview \
	console statusbar wintree events menu keys
CORE_OBJS = $(CORE:%=$(BUILD)/%.o)

$(CORE_OBJS): $(BUILD)/%.o: bridge/%.c bridge/core.h bridge/bridge.h bridge/menuhook.h | $(BUILD)
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -c -o $@ $<

$(BUILD)/wine-atspi.exe.so: $(CORE_OBJS) $(BUILD)/atspi.o
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -o $(BUILD)/wine-atspi.exe $^ $(GIO_LIBS) \
		-loleacc -lole32 -loleaut32 -lshell32 -lntdll

# Menu hook: a PE DLL (mingw), because Wine loads it into PE targets.
# 64-bit targets only for now.
MINGW_CC ?= x86_64-w64-mingw32-gcc

$(BUILD)/wine-atspi-hook.dll: bridge/menuhook.c bridge/menuhook.h | $(BUILD)
	$(MINGW_CC) -O2 -Wall -shared -static-libgcc -o $@ $< -luser32 -lkernel32

$(BUILD)/probe.exe.so: probe/probe.c | $(BUILD)
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -o $(BUILD)/probe.exe $< \
		-loleacc -lole32 -loleaut32 -lshell32

$(BUILD)/sendkeys.exe.so: tools/sendkeys.c | $(BUILD)
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -o $(BUILD)/sendkeys.exe $< -lshell32

$(BUILD)/controls.exe.so: tests/controls.c | $(BUILD)
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -o $(BUILD)/controls.exe $< -lcomctl32

prefix:
	test -d $(WINEPREFIX) || wineboot -i

run-probe: all prefix
	wine $(BUILD)/probe.exe.so

clean:
	rm -rf $(BUILD)

.PHONY: all prefix run-probe clean
