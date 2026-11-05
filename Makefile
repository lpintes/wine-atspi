# Build with winegcc in Arch WSL: make
# Run in the test prefix:         make run-probe

WINEGCC ?= winegcc
CFLAGS ?= -O2 -g -Wall
WINEPREFIX ?= $(HOME)/.wine-atspi
BUILD = build
# Win32 modules use Wine's msvcrt headers, not glibc.
WIN32_FLAGS = -mno-cygwin -mconsole

export WINEPREFIX

all: $(BUILD)/probe.exe.so $(BUILD)/sendkeys.exe.so

$(BUILD):
	mkdir -p $@

$(BUILD)/probe.exe.so: probe/probe.c | $(BUILD)
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -o $(BUILD)/probe.exe $< \
		-loleacc -lole32 -loleaut32 -lshell32

$(BUILD)/sendkeys.exe.so: tools/sendkeys.c | $(BUILD)
	$(WINEGCC) $(CFLAGS) $(WIN32_FLAGS) -o $(BUILD)/sendkeys.exe $< -lshell32

prefix:
	test -d $(WINEPREFIX) || wineboot -i

run-probe: all prefix
	wine $(BUILD)/probe.exe.so

clean:
	rm -rf $(BUILD)

.PHONY: all prefix run-probe clean
