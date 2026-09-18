# makefile - builds mmc on Linux and macOS (on Windows use build.bat)
#
#   make                    mmc (the shell) and mmc-term (the window)
#   make CC=gcc             any C11 compiler works, zig is only the default
#   make test               run the tests of the terminal core
#   make cross              every platform, into dist/ (needs zig)
#   make install PREFIX=~/mmc

CC= zig cc
CFLAGS= -std=c11 -O2 -Wall -Wextra -pedantic
LDFLAGS= -s
PREFIX= $(HOME)/mmc
ZIG= zig

BASE= mutil.c mpath.c mos.c
SRC= mmc.c mlex.c mexpand.c mexec.c mbuiltin.c mline.c $(BASE)
CORE= tgrid.c tvt.c ttheme.c tfont.c tdraw.c
TSRC= mterm.c tapp.c tpty.c twin32.c tx11.c tcocoa.c $(CORE) $(BASE)

# the window loads libX11 (Linux) or Cocoa (macOS) at run time with dlopen
TLIBS= -lm -ldl

all: mmc mmc-term

mmc: $(SRC) mmc.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o mmc $(SRC)

mmc-term: $(TSRC) mmc.h mterm.h stb_truetype.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o mmc-term $(TSRC) $(TLIBS)

test: ttest.c $(CORE) $(BASE) mterm.h
	$(CC) $(CFLAGS) -o ttest ttest.c $(CORE) $(BASE) -lm
	./ttest

cross: $(SRC) $(TSRC) mmc.h mterm.h
	mkdir -p dist
	for t in x86_64 aarch64; do \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-windows-gnu -o dist/mmc-shell-$$t-windows.exe $(SRC) mmc.rc -lshell32 || exit 1; \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-windows-gnu -o dist/mmc-term-$$t-windows.exe $(TSRC) mterm.rc -Wl,--subsystem,windows -lgdi32 -luser32 -lshell32 || exit 1; \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-linux-musl -static -o dist/mmc-$$t-linux $(SRC) || exit 1; \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-linux-gnu -o dist/mmc-term-$$t-linux $(TSRC) -ldl -lm || exit 1; \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-macos -o dist/mmc-$$t-macos $(SRC) || exit 1; \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-macos -o dist/mmc-term-$$t-macos $(TSRC) -lm || exit 1; \
	done
	rm -f dist/*.pdb

install: mmc mmc-term
	mkdir -p $(PREFIX)/usr/share/fonts
	cp mmc mmc-term LICENSE $(PREFIX)/
	cp Hack-Regular.ttf Hack-Bold.ttf Hack-Italic.ttf Hack-LICENSE.md $(PREFIX)/usr/share/fonts/

clean:
	rm -rf mmc mmc-term ttest mmc.exe mmc-shell.exe mmc-term.exe ttest.exe *.pdb dist

.PHONY: all test cross install clean
