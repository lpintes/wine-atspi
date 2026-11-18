# Build with winegcc in Arch WSL: make
# Run in the test prefix:         make run-probe

WINEGCC ?= winegcc
CFLAGS ?= -O2 -g -Wall
WINEPREFIX ?= $(HOME)/.wine-atspi
BUILD = build
# Win32 modules use Wine's msvcrt headers, not glibc.
# memset/memcpy/memmove bind to Wine's ms_abi ucrtbase, but calls that GCC
# makes up itself (loop to memset, struct copy) use the sysv ABI and crash.
# STRINGOP_FLAGS inline those; tests/abi-check.sh verifies the objects.
STRINGOP_FLAGS = -fno-tree-loop-distribute-patterns -minline-all-stringops \
	-mstringop-strategy=unrolled_loop
WIN32_FLAGS = -mno-cygwin -mconsole $(STRINGOP_FLAGS)

export WINEPREFIX

all: $(BUILD)/probe.exe.so $(BUILD)/sendkeys.exe.so $(BUILD)/controls.exe.so \
	$(BUILD)/msgbox-race.exe.so $(BUILD)/wine-atspi.exe.so $(BUILD)/wine-atspi-hook.dll

$(BUILD):
	mkdir -p $@

# AT-SPI side: host gcc against glibc and GLib, no Windows headers.
CC ?= gcc
GIO_CFLAGS := $(shell pkg-config --cflags gio-2.0)
GIO_LIBS := $(shell pkg-config --libs gio-2.0)

ATSPI_OBJS = $(BUILD)/atspi.o $(BUILD)/textrange.o

$(ATSPI_OBJS): $(BUILD)/%.o: bridge/%.c bridge/bridge.h bridge/textrange.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC $(GIO_CFLAGS) -c -o $@ $<

# Win32 core: winegcc, Windows headers only (see bridge/core.h).
CORE = main util object range text labels listview tab toolbar listbox treeview \
	console statusbar wintree events menu keys utf16
CORE_OBJS = $(CORE:%=$(BUILD)/%.o)

$(CORE_OBJS): $(BUILD)/%.o: bridge/%.c bridge/core.h bridge/bridge.h bridge/menuhook.h \
		bridge/utf16.h | $(BUILD)
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -c -o $@ $<

# Lua for application scripts (third_party/lua), part of the Win32 core.
# bridge/luauser.h adapts it to Winelib without touching its files.
LUA = lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump lfunc \
	lgc linit liolib llex lmathlib lmem loadlib lobject lopcodes loslib lparser \
	lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio
LUA_OBJS = $(LUA:%=$(BUILD)/lua/%.o)
LUA_FLAGS = -Ibridge -Ithird_party/lua -DLUA_USER_H='"luauser.h"'

$(BUILD)/lua:
	mkdir -p $@

$(LUA_OBJS): $(BUILD)/lua/%.o: third_party/lua/%.c bridge/luauser.h | $(BUILD)/lua
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) $(LUA_FLAGS) -c -o $@ $<

$(BUILD)/wine-atspi.exe.so: $(CORE_OBJS) $(LUA_OBJS) $(ATSPI_OBJS)
	sh tests/abi-check.sh $(CORE_OBJS) $(LUA_OBJS)
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -o $(BUILD)/wine-atspi.exe $^ $(GIO_LIBS) \
		-loleacc -lole32 -loleaut32 -lshell32 -lntdll

# Unit tests of the pure helpers, native, without Wine: make check
$(BUILD)/unit: tests/unit.c bridge/utf16.c bridge/utf16.h bridge/textrange.c \
		bridge/textrange.h | $(BUILD)
	$(CC) $(CFLAGS) -Ibridge $(GIO_CFLAGS) -o $@ tests/unit.c bridge/utf16.c \
		bridge/textrange.c $(GIO_LIBS)

check: $(BUILD)/unit
	$(BUILD)/unit

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

$(BUILD)/msgbox-race.exe.so: tests/msgbox-race.c | $(BUILD)
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -o $(BUILD)/msgbox-race.exe $< -lshell32

prefix:
	test -d $(WINEPREFIX) || wineboot -i

run-probe: all prefix
	wine $(BUILD)/probe.exe.so

clean:
	rm -rf $(BUILD)

.PHONY: all prefix run-probe check clean
