CC := x86_64-w64-mingw32-gcc
STRIP := x86_64-w64-mingw32-strip
PYTHON := python3
WIXL := wixl
MSIBUILD := msibuild
XXD := xxd
INPUT ?= shellcode.bin

CFLAGS := -Iinclude -Iresources -Ibuild -Os -DBOF -c -Wall -Wextra \
	-Wno-cast-function-type -Wno-unused-parameter

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

.PHONY: all clean package package-deps

all: dist/aie_spawn.x64.o

build dist:
	mkdir -p $@

$(INPUT):
	@echo "missing: $(INPUT)"
	@echo "place raw x64 shellcode in this directory as shellcode.bin"
	@exit 1

build/stage.h: scripts/embed.py $(INPUT) | build
	@command -v $(CC) >/dev/null || { echo "missing: $(CC)"; exit 1; }
	@command -v $(STRIP) >/dev/null || { echo "missing: $(STRIP)"; exit 1; }
	@command -v $(PYTHON) >/dev/null || { echo "missing: $(PYTHON)"; exit 1; }
	$(PYTHON) scripts/embed.py "$(INPUT)" $@

dist/aie_spawn.x64.o: src/main.c include/bof.h resources/package.h build/stage.h | dist
	$(CC) $(CFLAGS) $< -o $@
	$(STRIP) --strip-unneeded $@
	@echo "built $@"

build/bridge.exe: src/bridge.c | build
	$(CC) -municode -Os -s $< -o $@

build/package.msi: installer/package.wxs installer/CustomAction.idt build/bridge.exe | build
	$(WIXL) -a x64 -o $@ installer/package.wxs
	$(MSIBUILD) $@ -i installer/CustomAction.idt

package-deps:
	@command -v $(WIXL) >/dev/null || { echo "missing: $(WIXL)"; exit 1; }
	@command -v $(MSIBUILD) >/dev/null || { echo "missing: $(MSIBUILD)"; exit 1; }
	@command -v $(XXD) >/dev/null || { echo "missing: $(XXD)"; exit 1; }

package: package-deps build/package.msi
	$(XXD) -i -n package_data build/package.msi > resources/package.h

clean:
	rm -rf build dist
