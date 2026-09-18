# mmc

**mmc** stands for **M**arjon **M**angindo **C**ajocon.

A small, portable, git-bash style shell written from scratch in pure C11.
No dependencies, no installer: one executable that you can carry on any drive.

```
marjon@DESKTOP MMC ~/projects/app (main)
$ code .
```

## The idea

- I want a terminal that feels like Linux / git-bash: there is a `/home`,
  a `/usr`, an `/etc`, and `export PATH=...` works.
- The folder that holds the program becomes `/`. Everything (home, settings,
  tools) lives inside it, so the whole development setup is portable.
- To open the terminal I call `mmc`. Whatever I start from it — for example
  `code .` to open VS Code — carries the environment I set up in my config.

## Layout

The first run creates this next to the executable:

```
<mmc folder>/            this is "/"
  mmc.exe  mmc-shell.exe
  etc/profile            settings for everyone (PATH to the tools)
  home/<user>/.mmcrc     your own settings (aliases ...)
  home/<user>/.mmc_history
  usr/bin/               put your own programs and scripts here (in PATH)
  tmp/
```

## Build

The only tool needed is [zig](https://ziglang.org), used as a C compiler
(`zig cc`). Like Lua, all the C code is in the root folder.

```
build                 Windows: mmc.exe, mmc-shell.exe and mmc-term.exe
build cross           every platform, into dist\
build test            run the tests of the terminal core
build install D:\mmc  copy the programs into a folder
build clean
```

```
make                  Linux / macOS (make CC=gcc works too: it is plain C11)
make test
make cross
make install PREFIX=~/mmc
```

`build cross` makes Windows, Linux (static, musl) and macOS programs, each
for x86_64 and aarch64, from any of the three systems.

| File | What is in it |
|---|---|
| `mmc.h` | the one shared header |
| `mmc.c` | `main`, root folder, environment, prompt, read-run loop |
| `mlex.c` | tokenizer: words, quotes, operators |
| `mexpand.c` | `~`, `$VAR`, quotes, globbing (`* ? [a-z]`) |
| `mexec.c` | aliases, `; && \|\| &`, pipelines, redirections, PATH lookup |
| `mbuiltin.c` | builtin commands |
| `mline.c` | line editor: history, Tab completion, UTF-8 |
| `mpath.c` | Linux style paths on Windows (`/d/env` = `D:\env`) |
| `mos.c` | everything that differs between Windows and Linux/macOS |
| `mutil.c` | memory, string buffer, string vector |
| `mmc.rc`, `mmc.ico` | Windows only: the program icon and version details |
| `mterm.h` | **mmc-term**, the terminal window: its one shared header |
| `mterm.c` | `main` of mmc-term, command line |
| `tgrid.c` | screen model: cells, cursor, scroll region, alternate screen, scrollback |
| `tvt.c` | escape sequence (VT / xterm) parser |
| `ttheme.c` | the dark and light themes, `mmcterm.conf` |
| `tfont.c` | finds fonts, rasterizes and caches glyphs (uses `stb_truetype.h`) |
| `tdraw.c` | software renderer: text, box drawing, cursor, selection, scrollbar, menu |
| `tapp.c` | keys, mouse selection, clipboard, zoom, menu actions |
| `tpty.c` | the shell behind the window: ConPTY (Windows) or a pty (Linux, macOS) |
| `twin32.c` `tx11.c` `tcocoa.c` | one small window backend per system |
| `mterm.rc` | Windows only: icon and version details of mmc-term |
| `ttest.c` | tests of the terminal core, without a window |
| `stb_truetype.h` | font rasterizer by Sean Barrett, public domain, the only code not written here |
| `Hack-*.ttf`, `Hack-LICENSE.md` | the default font, [Hack](https://sourcefoundry.org/hack/) v3.003 (MIT / Bitstream Vera license); `install` copies it to `usr/share/fonts` |

## Install on Windows

1. `build install D:\mmc` (any folder on any drive works).
2. Add that folder to the Windows `PATH`
   (Settings → "Edit environment variables for your account" → Path → New).
3. Type `mmc-term` in Win+R or in the address bar of an Explorer window — the
   mmc window opens **in that folder**, like git-bash does. Inside another
   terminal (Windows Terminal, VS Code) type `mmc-shell` to get just the shell.

> **Why `mmc-shell` and not `mmc`?** Windows already has its own `mmc.exe`
> (Microsoft Management Console) in `C:\Windows\System32`, and System32 comes
> first in the PATH, so typing `mmc` outside the shell starts that one.
> `mmc.exe` and `mmc-shell.exe` are the same program. *Inside* the shell `mmc`
> always means this shell. On Linux and macOS there is no collision.

## mmc-term, the window

git-bash is bash inside the *mintty* window. mmc has its own window too:
`mmc-term` starts the mmc shell inside itself, in the colors of the logo
(navy `#0B1220`, blue `#2F9BFF`, green `#22D36B`).

```
mmc-term                     the mmc shell, in the current folder
mmc-term -e nvim notes.txt   another program instead of the shell
mmc-term --hold -e ...       keep the window when the program ends
mmc-term --theme light       dark or light, for this window only
```

| Keys and mouse | |
|---|---|
| Ctrl+Shift+C / Ctrl+Shift+V | copy / paste (also Ctrl+Insert / Shift+Insert, middle click pastes) |
| drag, double click, triple click | select text, a word, a line — selecting copies |
| wheel, Shift+PgUp / PgDn | scroll back (10 000 lines); Ctrl+Shift+Home / End: top / bottom |
| Ctrl + / Ctrl - / Ctrl 0, Ctrl+wheel | bigger, smaller, normal text |
| Ctrl+Shift+T | switch between the dark and the light theme (remembered) |
| Ctrl+Shift+wheel | see-through window: opacity 30 .. 100 % in steps of 5 (remembered; also in the menu) |
| F11 or Alt+Enter | full screen |
| Ctrl+Shift+N | new window |
| right click | menu |

On Windows mmc-term draws its own title bar: the logo, the title, and three
round buttons — yellow hides, green zooms (maximizes), red closes; a sign
appears in a button when the mouse is over it. Dragging the bar moves the
window, a double click zooms it, the edges still resize, and snapping to the
screen sides works as usual. `titlebar=native` in `mmcterm.conf` gives the
system title bar back. (Linux and macOS keep the system title bar for now.)

On a Mac the Command key does it: Cmd+C, Cmd+V, Cmd+N, Cmd+T, Cmd +/-.

Settings are in `/etc/mmcterm.conf` of the mmc folder (written on the first
start, every line is explained there): `theme`, `font`, `font_file`,
`font_size`, `cols`, `rows`, `padding`, `titlebar`, `bold` (`no` draws bold
text in the normal weight), `scrollback`, `cursor`, `cursor_blink`,
`opacity`, `copy_on_select`, `shell`, and your own colors (`bg`, `fg`,
`cursor_color`, `color0` … `color15`). The default font is **Hack**, which comes with mmc in `/usr/share/fonts` of
the mmc folder; fonts in that folder are found first, so a font travels with
mmc. Without it mmc-term takes Cascadia / Consolas (Windows), DejaVu Sans Mono
(Linux) or Menlo (macOS). When you copy a program from `dist/` by hand, copy
the three `Hack-*.ttf` files to `usr/share/fonts` too.

How it is made: one shared core draws the *whole* terminal — text, box drawing
characters, cursor, selection, scrollbar, even the menu — into a plain pixel
buffer, and a small backend per system only shows that picture and passes the
keys on. So it looks the same everywhere. X11 and Cocoa are loaded at run time
(`dlopen`), which is why zig can cross compile the Linux and macOS programs
from Windows without any SDK.

| System | State |
|---|---|
| Windows 10 1809+ / 11 | **tested**: typing, history, Tab, Ctrl-C, resize, scrollback, selection and clipboard, menu, themes, nvim, large outputs |
| Linux (X11, or Wayland through XWayland) | **experimental**: compiles and links for x86_64 and aarch64, shares the tested core, but the X11 backend (`tx11.c`) has not been run yet |
| macOS | **untested**: compiles and links for x86_64 and aarch64; the Cocoa backend (`tcocoa.c`) has never been run |

Testing on Linux or macOS: copy `mmc-<cpu>-<os>` as `mmc` and
`mmc-term-<cpu>-<os>` as `mmc-term` into one folder, `chmod +x` both, run
`./mmc-term` from a terminal, and report what is printed there and what the
window does (does it open, is the text right, do keys, mouse, copy/paste and
resize work). `./mmc-term --render-test out.bmp` draws a sample to an image
without opening any window: if that picture is right, the core and the fonts
work and only the window backend is left to look at.

Not there (yet): tabs, re-wrapping text on resize, mouse reporting to programs
(the mouse in vim), color emoji, ligatures, search, clickable links.

## Settings

`/etc/profile` is read once when the shell starts; `~/.mmcrc` is read by every
interactive shell. The syntax is a small bash-like subset:
`export NAME=value`, `alias name='value'`, `source file`, `# comments`.

Example `/etc/profile` for a tools folder on the same drive as mmc
(`$MMC_DRIVE` is the drive mmc is on, e.g. `/d`, so the file keeps working
when the drive letter changes on another PC):

```sh
E=$MMC_DRIVE/env
export JAVA_HOME=$E/java/jdk-21.0.11
export ANDROID_HOME=$E/Android/Sdk
export GOROOT=$E/go_lang/go1.23.12.windows-amd64/go
export PATH="$E/zig:$E/tcc:$E/esbuild:$E/bun/bun-windows-x64:$PATH"
export PATH="$E/node/node-v22.22.2-win-x64:$GOROOT/bin:$JAVA_HOME/bin:$PATH"
export PATH="$E/python/python-3.9.10.amd64:$E/python/python-3.9.10.amd64/Scripts:$PATH"
export PATH="$E/nvim-win64/bin:$E/ffmpeg/bin:$E/flutter/bin:$E/gradle/gradle-8.14.2/bin:$PATH"
export PATH="$ANDROID_HOME/platform-tools:$PATH"
# the GNU tools that come with git (grep, less, sed, ssh ...), last in line
export PATH="$PATH:$MMC_DRIVE/PortableGit/cmd:$MMC_DRIVE/PortableGit/usr/bin"
export EDITOR=nvim
```

**Prompt.** The default is the two line Parrot OS style, with blue connector
lines, and a red `[✗]` after a command that failed:

```
┌─[marjon@DESKTOP]─[MMC]─[~/w/mmc]─[master]
└──╼ $
```

`export MMC_PROMPT=classic` in `/etc/profile` or `~/.mmcrc` gives the one
line git-bash style prompt instead.

**Banner.** An interactive shell greets with `MMC` in big green letters and
the name of the developer. A file `/etc/banner` replaces the big letters with
your own drawing (plain text or ANSI colors, for example a picture converted
to colored half blocks); an empty `/etc/banner` switches the banner off.

Variables set by mmc: `HOME`, `USER`, `HOSTNAME`, `SHELL`, `MMC_ROOT`,
`MMC_DRIVE` (Windows), `MMC_LEVEL`, and `PATH` starts with `/usr/bin:~/bin`.

## Paths (Windows)

| You type | It means |
|---|---|
| `/` | the mmc folder |
| `/home/me`, `~` | `<mmc folder>\home\me` |
| `/d/env/zig` | `D:\env\zig` |
| `/dev/null` | `NUL` |

Programs started by mmc are normal Windows programs. When one is started, the
arguments and environment values that look like Linux paths are turned back
into Windows paths (`HOME`, `PATH`, `GOROOT=/d/env/...`, `--out=/tmp/x`), so
git, node, go, VS Code ... all get what they expect. Switches such as `/c` or
`/all` are left alone; write `//x` when you really need a literal `/x`.
Use forward slashes; a backslash only escapes shell characters (`\ `, `\*`),
so `C:\Users\me` still works as it is.

On Linux and macOS paths are real paths and nothing is translated; the
settings are `$MMC_ROOT/etc/profile` and `$MMC_ROOT/home/<user>/.mmcrc`.

## What the shell can do

```
cmd | cmd      a && b     a || b     a ; b     cmd &
> file   >> file   < file   2> file   2>> file   2>&1   >&2   &> file
'text'   "text $VAR"   $VAR   ${VAR}   ${VAR:-default}   $?   $$   $1 $# $@
~   *.c   file?.txt   [a-z]*   NAME=value   NAME=value command
mmc -c 'command'      mmc script.mmc arg1 arg2      #!/usr/bin/env mmc
```

Builtins: `cd` (`cd -`), `pwd`, `echo`, `export`, `unset`, `alias`, `unalias`,
`source` / `.`, `which` / `type`, `history`, `help`, `exit`, `true`, `false`.
Small fallbacks, used only when no real program with that name is in the PATH:
`ls` (`-a -l`), `cat`, `clear`, `mkdir` (`-p`), `env`.

Keys: Tab completes commands and files, Up/Down history, Left/Right,
Ctrl-Left/Right by word, Home/End, Ctrl-A/E/K/U/W/L, Ctrl-C drops the line,
Ctrl-D leaves. On Windows `.exe .com .cmd .bat` are found without typing the
extension, and scripts starting with `#!` run with their interpreter.

Not there (yet): `if` / `for` / functions, `$(command)`, job control (`fg`,
`bg`), and mmc is a shell, not a terminal window — it runs inside Windows
Terminal, the classic console, or any Linux/macOS terminal.

## How git-bash does it

git-bash is three things: the **MSYS2 runtime** (`msys-2.0.dll`, a fork of
Cygwin that emulates POSIX on Windows), **GNU bash** compiled against it, and
the **mintty** terminal. `/` is the install folder, `/c/...` are the drives,
and paths are converted when a Windows program is started.

mmc keeps that idea but has no emulation layer: it is one native program that
calls the Win32 API on Windows and the POSIX API on Linux/macOS (see `mos.c`).

## Tested

Windows 11: builds with zero warnings (`-std=c11 -Wall -Wextra -pedantic`);
scripts, pipes, redirections, globbing, aliases, batch files, `#!` scripts,
nested shells, UTF-8 and the interactive line editor were exercised.
Linux and macOS: compile cleanly for x86_64 and aarch64, not yet run.
Windows 10 or newer is needed (the console must understand VT sequences).

## License

mmc is free software under the [MIT License](LICENSE).
Copyright (c) 2026 Marjon Mangindo Cajocon.

You may use, copy, change and share it, also commercially, as long as the
copyright notice and the license text stay with it. It comes without warranty.

Two things in this folder were made by others and keep their own license:

| Part | By | License |
|---|---|---|
| `stb_truetype.h` | Sean Barrett | MIT or public domain, at your choice; the text is at the end of the file |
| `Hack-*.ttf` | Source Foundry Authors, Bitstream Inc. | MIT and Bitstream Vera License, see `Hack-LICENSE.md`; the font may travel with a program, it may not be sold on its own |
