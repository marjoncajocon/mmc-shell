#!/bin/sh
# build.sh - builds mmc with zig (https://ziglang.org), used as a C compiler;
# build.bat's twin for a bash-like shell: mmc itself, git-bash, Linux, macOS
#
#   ./build.sh                 mmc, mmc-shell and mmc-term for this PC
#   ./build.sh cross           every platform, into dist/
#   ./build.sh test            run the tests of the terminal core
#   ./build.sh install DIR     copy the programs into DIR (add DIR to your PATH)
#   ./build.sh release         the downloads of a release, into release/
#   ./build.sh clean
#
# On Windows mmc.exe and mmc-shell.exe are the same program: the shell (System32
# has another mmc.exe, Microsoft Management Console). On Linux and macOS the
# native build uses CC when it is set (CC=gcc ./build.sh), as the makefile does.

cd "$(dirname "$0")" || exit 1

ZIG=zig
command -v zig >/dev/null 2>&1 || ZIG=/d/env/zig/zig.exe

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows*) WIN=1; X=.exe ;;
  *) WIN=0; X= ;;
esac

CFLAGS="-std=c11 -O2 -s -Wall -Wextra -pedantic"
# the default font: JetBrains Mono with ligatures, the Powerline and Nerd Font icons
FONTS="JetBrainsMonoNerdFontMono-Regular.ttf JetBrainsMonoNerdFontMono-Bold.ttf JetBrainsMonoNerdFontMono-Italic.ttf JetBrainsMonoNerdFontMono-BoldItalic.ttf JetBrainsMonoNerdFont-OFL.txt JetBrainsMonoNerdFont-README.md"
BASE="mutil.c mpath.c mos.c"
TOOLS="ctool.c cfile.c cfind.c ctext.c cgrep.c csed.c csys.c cdiff.c carch.c czip.c cawk.c chash.c cinfo.c cmore.c"
SRC="mmc.c mparse.c mexpand.c mpattern.c marith.c mregex.c mvar.c mexec.c mjobs.c mbuiltin.c mbvars.c mbio.c mbtest.c mline.c mcomp.c $TOOLS $BASE"
CORE="tgrid.c tvt.c ttheme.c tfont.c tshape.c tdraw.c"
TSRC="mterm.c tapp.c tpty.c twin32.c tx11.c tcocoa.c $CORE $BASE"
# Windows only: the .rc files put the icon (mmc.ico) and version details in
WINRES="mmc.rc -lshell32"
TWINRES="mterm.rc -Wl,--subsystem,windows -lgdi32 -luser32 -lshell32"

# copies $1 to $2. A program that is running cannot be overwritten, but
# Windows lets us rename it: the old one moves aside, the new one fits.
put () {
  cp -f "$1" "$2" 2>/dev/null && return 0
  mv -f "$2" "$2.old$$" || return 1
  cp -f "$1" "$2" || return 1
  echo "  ($(basename "$2") is running: it keeps the old version until you restart it)"
}

native () {
  if [ "$WIN" = 1 ]; then
    $ZIG cc $CFLAGS -target x86_64-windows-gnu -o mmc.exe $SRC $WINRES || exit 1
    cp -f mmc.exe mmc-shell.exe || exit 1
    $ZIG cc $CFLAGS -target x86_64-windows-gnu -o mmc-term.exe $TSRC $TWINRES || exit 1
    echo "built mmc.exe, mmc-shell.exe and mmc-term.exe"
  else
    ${CC:-$ZIG cc} $CFLAGS -o mmc $SRC -lpthread || exit 1
    # the window loads libX11 (Linux) or Cocoa (macOS) at run time with dlopen
    ${CC:-$ZIG cc} $CFLAGS -o mmc-term $TSRC -lm -ldl -lpthread || exit 1
    echo "built mmc and mmc-term"
  fi
}

run_test () {
  if [ "$WIN" = 1 ]; then
    $ZIG cc $CFLAGS -target x86_64-windows-gnu -o ttest.exe ttest.c $CORE $BASE -lgdi32 -lshell32 || exit 1
  else
    ${CC:-$ZIG cc} $CFLAGS -o ttest ttest.c $CORE $BASE -lm -lpthread || exit 1
  fi
  ./ttest$X
}

cross () {
  mkdir -p dist
  for t in x86_64 aarch64; do
    echo "$t-windows"
    $ZIG cc $CFLAGS -target $t-windows-gnu -o dist/mmc-shell-$t-windows.exe $SRC $WINRES || exit 1
    $ZIG cc $CFLAGS -target $t-windows-gnu -o dist/mmc-term-$t-windows.exe $TSRC $TWINRES || exit 1
    echo "$t-linux"
    # static musl: the same program runs on any Linux, and on Android (Termux, adb shell)
    $ZIG cc $CFLAGS -target $t-linux-musl -static -o dist/mmc-$t-linux $SRC || exit 1
    # the window loads libX11 at run time: that needs the dynamic C library
    $ZIG cc $CFLAGS -target $t-linux-gnu -o dist/mmc-term-$t-linux $TSRC -ldl -lm -lpthread || exit 1
    echo "$t-macos"
    $ZIG cc $CFLAGS -target $t-macos -o dist/mmc-$t-macos $SRC || exit 1
    $ZIG cc $CFLAGS -target $t-macos -o dist/mmc-term-$t-macos $TSRC -lm || exit 1
  done
  echo "arm-linux (older 32 bit Android phones)"
  $ZIG cc $CFLAGS -target arm-linux-musleabihf -static -o dist/mmc-arm-linux $SRC || exit 1
  rm -f dist/*.pdb
  echo "done, see dist/"
}

install () {
  if [ -z "$1" ]; then
    echo "usage: ./build.sh install DIR"
    exit 2
  fi
  [ -f mmc-term$X ] || native
  mkdir -p "$1/usr/share/fonts" || exit 1
  rm -f "$1"/*.old* 2>/dev/null
  put mmc$X "$1/mmc$X" || exit 1
  [ "$WIN" = 1 ] && { put mmc.exe "$1/mmc-shell.exe" || exit 1; }
  put mmc-term$X "$1/mmc-term$X" || exit 1
  cp -f LICENSE "$1/LICENSE"
  # the JetBrains Mono Nerd Font travels with mmc: mmc-term looks in usr/share/fonts first
  for f in $FONTS; do cp -f "$f" "$1/usr/share/fonts/$f" || exit 1; done
  echo "installed the programs in $1"
  echo "add $1 to your PATH, then type: mmc-term  (the window)  or  mmc-shell"
}

# One archive per system, named mmc-shell-VERSION-SYSTEM, each with a folder
# of that name inside: only the programs, LICENSE and the font (no source).
# Windows gets .zip, Linux and macOS .tar.gz. On Windows both are written by
# Windows' own tar (bsdtar), the .tar.gz from an mtree list that gives the
# programs their x bit, which a file on a Windows disk does not have.
# SHA256SUMS.txt lets people check a download: sha256sum -c SHA256SUMS.txt
release () {
  BSDTAR=
  if [ "$WIN" = 1 ]; then
    BSDTAR="${SYSTEMROOT:-${SystemRoot:-C:/Windows}}/System32/tar.exe"
    [ -f "$BSDTAR" ] || { echo "release: no $BSDTAR (Windows 10 and later have it)"; exit 1; }
  fi
  VER=$(sed -n 's/^#define MMC_VERSION[ \t]*"\(.*\)".*/\1/p' mmc.h)
  if [ -z "$VER" ]; then
    echo "cannot read MMC_VERSION from mmc.h"
    exit 1
  fi
  ( run_test ) || exit 1
  ( cross ) || exit 1
  rm -rf release
  mkdir -p release
  pkg_win x86_64 x64
  pkg_win aarch64 arm64
  pkg_unix x86_64 linux linux-x64
  pkg_unix aarch64 linux linux-arm64
  pkg_unix x86_64 macos macos-x64
  pkg_unix aarch64 macos macos-arm64
  pkg_unix arm linux linux-arm shell
  if command -v sha256sum >/dev/null 2>&1; then
    (cd release && sha256sum * | sed 's/ [*]/  /' > SHA256SUMS.txt) || exit 1
  else
    (cd release && shasum -a 256 * > SHA256SUMS.txt) || exit 1
  fi
  echo "mmc $VER is ready in release/"
  ls release
}

# $1 = zig's name of the CPU, $2 = the name in the archive
pkg_win () {
  PKG=mmc-shell-$VER-windows-$2
  D=release/$PKG
  mkdir -p "$D/usr/share/fonts"
  cp -f dist/mmc-shell-$1-windows.exe "$D/mmc.exe" || exit 1
  cp -f dist/mmc-shell-$1-windows.exe "$D/mmc-shell.exe" || exit 1
  cp -f dist/mmc-term-$1-windows.exe "$D/mmc-term.exe" || exit 1
  cp -f LICENSE "$D/LICENSE"
  for f in $FONTS; do cp -f "$f" "$D/usr/share/fonts/$f"; done
  if [ -n "$BSDTAR" ]; then
    "$BSDTAR" -a -cf "release/$PKG.zip" -C release "$PKG" || exit 1
  else
    (cd release && zip -qr "$PKG.zip" "$PKG") || exit 1
  fi
  rm -rf "$D"
}

# $1 = CPU, $2 = linux or macos, $3 = the name in the archive,
# $4 = "shell" for the shell alone (32 bit ARM has no mmc-term)
pkg_unix () {
  PKG=mmc-shell-$VER-$3
  if [ -n "$BSDTAR" ]; then
    M=release/$PKG.mtree
    {
      echo "#mtree"
      echo "$PKG/mmc type=file mode=0755 contents=dist/mmc-$1-$2"
      [ "$4" != shell ] && echo "$PKG/mmc-term type=file mode=0755 contents=dist/mmc-term-$1-$2"
      echo "$PKG/LICENSE type=file mode=0644 contents=LICENSE"
      if [ "$4" != shell ]; then
        for f in $FONTS; do echo "$PKG/usr/share/fonts/$f type=file mode=0644 contents=$f"; done
      fi
    } > "$M"
    "$BSDTAR" -czf "release/$PKG.tar.gz" "@$M" || exit 1
    rm -f "$M"
    return 0
  fi
  D=release/$PKG
  mkdir -p "$D"
  cp -f dist/mmc-$1-$2 "$D/mmc" || exit 1
  if [ "$4" != shell ]; then
    cp -f dist/mmc-term-$1-$2 "$D/mmc-term" || exit 1
    mkdir -p "$D/usr/share/fonts"
    for f in $FONTS; do cp -f "$f" "$D/usr/share/fonts/$f"; done
  fi
  cp -f LICENSE "$D/LICENSE"
  chmod 755 "$D"/mmc*
  (cd release && tar -czf "$PKG.tar.gz" "$PKG") || exit 1
  rm -rf "$D"
}

case "$1" in
  "") native ;;
  cross) cross ;;
  test) run_test; exit $? ;;
  install) install "$2" ;;
  release) release ;;
  clean) rm -rf mmc mmc-term ttest mmc.exe mmc-shell.exe mmc-term.exe ttest.exe mmc.pdb mmc-term.pdb ttest.pdb dist ;;
  *)
    echo "usage: ./build.sh [cross | test | install DIR | release | clean]"
    exit 2
    ;;
esac
exit 0
