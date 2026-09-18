# makefile - builds mmc on Linux and macOS (on Windows use build.bat)
#
#   make                    mmc for this machine
#   make CC=gcc             any C11 compiler works, zig is only the default
#   make cross              every platform, into dist/ (needs zig)
#   make install PREFIX=~/mmc

CC= zig cc
CFLAGS= -std=c11 -O2 -Wall -Wextra -pedantic
LDFLAGS= -s
PREFIX= $(HOME)/mmc
ZIG= zig

SRC= mmc.c mlex.c mexpand.c mexec.c mbuiltin.c mline.c mpath.c mos.c mutil.c

mmc: $(SRC) mmc.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o mmc $(SRC)

cross: $(SRC) mmc.h
	mkdir -p dist
	for t in x86_64 aarch64; do \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-windows-gnu -o dist/mmc-shell-$$t-windows.exe $(SRC) -lshell32 || exit 1; \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-linux-musl -static -o dist/mmc-$$t-linux $(SRC) || exit 1; \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-macos -o dist/mmc-$$t-macos $(SRC) || exit 1; \
	done
	rm -f dist/*.pdb

install: mmc
	mkdir -p $(PREFIX)
	cp mmc $(PREFIX)/mmc

clean:
	rm -rf mmc mmc.exe mmc.pdb dist

.PHONY: cross install clean
