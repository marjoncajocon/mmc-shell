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
  etc/user               the user name: the same home on every PC
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
bash tests/run.sh     the bash compatibility tests (mmc tests/run.sh works too)
build clean
```

```
make                  Linux / macOS (make CC=gcc works too: it is plain C11)
make test
make cross
make install PREFIX=~/mmc
```

`build cross` makes Windows, Linux (static, musl) and macOS programs, each
for x86_64 and aarch64, from any of the three systems, and `mmc-arm-linux`
for older 32 bit phones. The static Linux programs are also the Android ones:
they run in Termux and in `adb shell` as they are (`$TMPDIR` is used there,
Android has no `/tmp`). mmc-term needs X11 or Cocoa, so Android gets the shell.

| File | What is in it |
|---|---|
| `mmc.h` | the one shared header |
| `mmc.c` | `main`, options, root folder, environment, prompt, read loop, `--check` |
| `mparse.c` | lexer and parser: the bash grammar into a syntax tree |
| `mexpand.c` | expansion: braces, `~`, `${...}`, `$( )`, `$(( ))`, splitting, quotes |
| `mpattern.c` | patterns (`* ? [ ]`, extglob) and filename globbing (`**`) |
| `marith.c` | arithmetic: `$(( ))`, `(( ))`, `let`, subscripts |
| `mregex.c` | extended regular expressions for `[[ x =~ re ]]` |
| `mvar.c` | variables: scopes, attributes, arrays, the environment |
| `mexec.c` | running the tree: pipelines, subshells, redirections, functions, traps |
| `mjobs.c` | background jobs; `jobs wait kill trap umask ...` |
| `mbuiltin.c` | the builtin table, `cd pushd type command source ...`, aliases |
| `mbvars.c` | `declare local export readonly unset set shopt shift getopts let` |
| `mbio.c` | `echo printf read mapfile` |
| `mbtest.c` | `test`, `[`, `[[ ]]` |
| `mline.c` | line editor: history, Ctrl-R search, Tab completion, UTF-8 |
| `mcomp.c` | programmable completion: `complete`, `compgen`, `compopt` |
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
| `HackNerdFontMono-*.ttf`, `HackNerdFont-LICENSE.md`, `HackNerdFont-README.md` | the default font: [Hack](https://sourcefoundry.org/hack/) v3.003 with the [Nerd Fonts](https://www.nerdfonts.com) v3.5.1 Powerline and icon glyphs (MIT / Bitstream Vera; the icon sets' licenses are in the README); `install` copies it to `usr/share/fonts` |
| `tests/compat/` | 420 small bash scripts with what real bash printed for them; `tests/run.sh` compares mmc |

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
mmc-term --font-size 11      text size in points, for this window only
mmc-term --render-test a.bmp draw a sample into an image, no window
```

| Keys and mouse | |
|---|---|
| Ctrl+Shift+C / Ctrl+Shift+V | copy / paste (also Ctrl+Insert / Shift+Insert, middle click pastes) |
| drag, double click, triple click | select text, a word, a line — selecting copies |
| wheel, Shift+PgUp / PgDn | scroll back (10 000 lines); Ctrl+Shift+Home / End: top / bottom |
| Ctrl + / Ctrl - / Ctrl 0, Ctrl+wheel | bigger, smaller, normal text |
| Ctrl+Shift+T / Ctrl+Shift+W | new tab / close tab (closing the last one closes the window) |
| Ctrl+Tab / Ctrl+Shift+Tab | next / previous tab (also: click a tab, or the wheel over the tabs) |
| Ctrl+Shift+L | switch between the dark and the light theme (remembered) |
| Ctrl+Shift+wheel | see-through window: opacity 30 .. 100 % in steps of 5 (remembered; also in the menu) |
| F11 or Alt+Enter | full screen |
| Ctrl+Shift+N | new window |
| right click | menu |

**Tabs.** Every tab is its own shell, started in the folder of the tab you
are in. The tab bar shows up with the second tab — the window grows by its
height, so no shell loses a line — and goes away again with the last but
one. A tab shows the title its program set, or the name you give it: double
click the tab (or *Rename tab* in the menu), type, Enter keeps it, Esc drops
it, and an empty name gives the program's title back. Your name stays when the
shell changes its title, and it is also the window title. The `x` (or a middle click)
closes it, `+` opens one, and a green dot says that a tab you are not looking
at printed something. When the program of a tab ends (`exit`), the tab
closes; with `--hold` the first tab stays to show how it ended.

On Windows mmc-term draws its own title bar: the logo, the title, and three
round buttons — yellow hides, green zooms (maximizes), red closes; a sign
appears in a button when the mouse is over it. Dragging the bar moves the
window, a double click zooms it, the edges still resize, and snapping to the
screen sides works as usual. `titlebar=native` in `mmcterm.conf` gives the
system title bar back. (Linux and macOS keep the system title bar for now.)

On a Mac the Command key does it: Cmd+C, Cmd+V, Cmd+N, Cmd+T (new tab),
Cmd+W (close tab), Cmd+L (theme), Cmd +/-.

Settings are in `/etc/mmcterm.conf` of the mmc folder (written on the first
start, every line is explained there): `theme`, `font`, `font_file`,
`font_size`, `cols`, `rows`, `padding`, `titlebar`, `bold` (`no` draws bold
text in the normal weight), `scrollback`, `cursor`, `cursor_blink`,
`opacity`, `copy_on_select`, `shell`, and your own colors (`bg`, `fg`,
`cursor_color`, `color0` … `color15`), `font_size` (points, 9 like git-bash)
and `font_smoothing`.

**Fonts.** The default font is **Hack** in its Nerd Font version, which comes
with mmc in `/usr/share/fonts` of the mmc folder; fonts in that folder are
found first, so a font travels with mmc. It has the Powerline symbols and the
Nerd Font icons (git branch, folders, languages, systems), and it is also the
fallback for icons when another font is chosen. Without it mmc-term takes
Cascadia / Consolas (Windows), DejaVu Sans Mono (Linux) or Menlo (macOS). When
you copy a program from `dist/` by hand, copy the `HackNerdFontMono-*.ttf`
files to `usr/share/fonts` too.

**Smooth text.** On Windows the letters are drawn by Windows itself (GDI),
hinted to the pixel grid and ClearType filtered, like in every other Windows
program - this is what makes git-bash's window sharp at small sizes, and
mmc-term does the same (`font_smoothing=cleartype`, the default; `gray` is
the same without the colored edges; `stb` is the built-in rasterizer that
Linux and macOS use). Powerline arrows, rounds and slants (U+E0B0..E0BF), box
drawing and block characters are drawn as geometry, so they fill their cells
exactly and prompt segments join without seams at every size and zoom.

How it is made: one shared core draws the *whole* terminal — text, box drawing
characters, cursor, selection, scrollbar, even the menu — into a plain pixel
buffer, and a small backend per system only shows that picture and passes the
keys on. So it looks the same everywhere. X11 and Cocoa are loaded at run time
(`dlopen`), which is why zig can cross compile the Linux and macOS programs
from Windows without any SDK.

| System | State |
|---|---|
| Windows 10 1809+ / 11 | **tested**: typing, history, Tab, Ctrl-C, resize, scrollback, selection and clipboard, menu, themes, tabs, nvim, large outputs |
| Linux (X11, or Wayland through XWayland) | **experimental**: compiles and links for x86_64 and aarch64, shares the tested core, but the X11 backend (`tx11.c`) has not been run yet |
| macOS | **untested**: compiles and links for x86_64 and aarch64; the Cocoa backend (`tcocoa.c`) has never been run |

Testing on Linux or macOS: copy `mmc-<cpu>-<os>` as `mmc` and
`mmc-term-<cpu>-<os>` as `mmc-term` into one folder, `chmod +x` both, run
`./mmc-term` from a terminal, and report what is printed there and what the
window does (does it open, is the text right, do keys, mouse, copy/paste and
resize work). `./mmc-term --render-test out.bmp` draws a sample to an image
without opening any window: if that picture is right, the core and the fonts
work and only the window backend is left to look at.

The mouse reaches the programs: when one asks for it (`?1000`, `?1002`,
`?1003`, with the SGR reports of `?1006`), clicks, drags and the wheel are
sent on, so the mouse works in vim, htop and lazygit. Holding **Shift**
takes the mouse back for selecting text, as in xterm.

Programs can also ask the window things: `OSC 10/11/12` for the text,
background and cursor color (nvim reads them to pick its theme, and setting
them works too), `OSC 4` for a palette color, `OSC 52` to put something in
the clipboard, and `OSC 7`, with which the shell says where it is, so
Ctrl+Shift+N opens the new window (and Ctrl+Shift+T the new tab) in the same
folder.

Not there (yet): re-wrapping text on resize, color emoji, ligatures, search,
clickable links, moving tabs by dragging.

## Settings

`/etc/profile` is read once when the shell starts; `~/.mmcrc` is read by every
interactive shell. Both are bash scripts: `export`, `alias`, `source`, `if`,
functions, `$(command)` ... everything in "What the shell can do" works there.

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

**One home on every PC.** mmc is carried around, and the login name is
different on each PC — the home folder must not be. The name of the user is
therefore kept in `/etc/user` (written on the first start) and `HOME` is
`/home/<that name>` wherever the folder is plugged in; `.mmcrc`, the history
and `.gitconfig` come along. Edit `/etc/user` to change the name.

**Prompt.** The default is the two line Parrot OS style, with blue connector
lines, and a red `[✗]` after a command that failed:

```
┌─[marjon@DESKTOP]─[MMC]─[~/w/mmc]─[master]
└──╼ $
```

`export MMC_PROMPT=...` in `/etc/profile` or `~/.mmcrc` changes it:
`classic` is the one line git-bash style prompt, `powerline` draws colored
segments with arrows and the git branch icon (it needs the Hack Nerd Font,
which mmc-term has), and `ps1` uses your own `$PS1` with bash's `\u \h \w \$`
escapes. A continued command line shows `$PS2` (`> `).

**Banner.** An interactive shell greets with `MMC` in big green letters and
the name of the developer. A file `/etc/banner` replaces the big letters with
your own drawing (plain text or ANSI colors, for example a picture converted
to colored half blocks); an empty `/etc/banner` switches the banner off.

Variables set by mmc: `HOME`, `USER`, `HOSTNAME`, `SHELL`, `MMC_ROOT`,
`MMC_DRIVE` (Windows), `MMC_LEVEL`, `MMC_VERSION`, and `PATH` starts with
`/usr/bin:~/bin`; like bash also `BASH_VERSION` (scripts check it before they
use bash features), `BASH_VERSINFO`, `OSTYPE` (`msys` on Windows, as in
git-bash), `MACHTYPE`, `HOSTTYPE`, `PPID`, `UID`, `SHLVL`, `PWD`, `OLDPWD`,
`RANDOM`, `SECONDS`, `LINENO`, `EPOCHSECONDS`, `PIPESTATUS`, `FUNCNAME`,
`BASH_SOURCE`, `BASH_LINENO`, `BASH_COMMAND`, `BASH_REMATCH`, `DIRSTACK`,
`BASH_SUBSHELL`, `IFS`, `PS1`-`PS4`.

Variables mmc reads: `PROMPT_COMMAND` (run before every prompt; an array
works too), `HISTFILE`, `HISTSIZE`, `HISTFILESIZE` and `HISTCONTROL`
(`ignorespace`, `ignoredups`, `ignoreboth`, `erasedups`; unset means
`ignoreboth`, which is what mmc always did).

## Paths (Windows)

| You type | It means |
|---|---|
| `/` | the mmc folder |
| `/home/me`, `~` | `<mmc folder>\home\me` |
| `/d/env/zig` | `D:\env\zig` |
| `/tmp` | the Windows temp folder (`%TEMP%`), the same as git-bash's |
| `/dev/null` | `NUL` |

Programs started by mmc are normal Windows programs. When one is started, the
arguments and environment values that look like Linux paths are turned back
into Windows paths (`HOME`, `PATH`, `GOROOT=/d/env/...`, `--out=/tmp/x`), so
git, node, go, VS Code ... all get what they expect. Switches such as `/c` or
`/all` are left alone; write `//x` when you really need a literal `/x`.
git-bash's own programs (`sed`, `grep`, `dirname` ... from PortableGit's
`usr/bin`) read Linux paths themselves: for them only paths that exist in the
mmc folder are converted, `/tmp` and `/c/...` go as they are, and `* ? [`
arguments are quoted so they do not glob a second time. `/bin/rm` and
`/usr/bin/env` find the program of that name in the PATH.
Use forward slashes; a backslash only escapes shell characters (`\ `, `\*`),
so `C:\Users\me` still works as it is.

On Linux and macOS paths are real paths and nothing is translated; the
settings are `$MMC_ROOT/etc/profile` and `$MMC_ROOT/home/<user>/.mmcrc`.

## What the shell can do

mmc runs bash scripts. `tests/compat` has 420 cases taken from how the scripts
on a developer PC really use bash (git, gradle, flutter, npm, emsdk, the
Android SDK: see `tests/compat/RESEARCH.md`); mmc gives the same output and
exit status as bash 5.3 for all of them.

```
a | b   a |& b   a && b   a || b   a ; b   a &   ! a   time a   ( a )   { a; }
if/elif/else/fi   while/until   for x in ...   for ((i=0; i<n; i++))   case/esac
select   f() { ...; }   function f { ...; }   return   break n   continue n
[[ $a == x* && -f $f || $s =~ ^v([0-9]+) ]]   (( i++ ))   let   $(( 2**10 ))
> >> < <> >| 2> 2>&1 >&2 &> &>> n>&m n<&- n>&m- <<EOF <<-EOF <<'EOF' <<<
exec 3>file   {fd}>file   $(cmd)   `cmd`   $(< file)   <(cmd)   >(cmd)
'x'  "x $v"  $'\n\t'  $"x"  \x   {a,b}{1..3}  {01..10..2}  ~  ~+  ~-
$v ${v} ${v:-x} ${v:=x} ${v:?x} ${v:+x} ${#v} ${v#p} ${v##p} ${v%p} ${v%%p}
${v/p/r} ${v//p/r} ${v/#p/r} ${v/%p/r} ${v:1:3} ${v^^} ${v,,} ${!v} ${!p*}
${v@Q} ${v@U} ${v@A}   a=(1 2 3) a+=(4) a[i]=x ${a[@]} ${#a[@]} ${!a[@]}
declare -A m=([k]=v)   declare -i -l -u -r -x -n -g   local   readonly
* ? [a-z] [!x] [[:alpha:]] **  ?(x) *(x) +(x) @(x|y) !(x)   (shopt -s extglob)
$? $$ $! $# $@ $* "$@" $- $0 $1 ${10} $_   IFS, set -euxo pipefail, trap
```

Builtins (the same as bash's): `: . [ alias bg bind break builtin caller cd
command compgen complete compopt continue declare dirs disown echo enable eval
exec exit export false fc fg getopts hash help history jobs kill let local
logout mapfile popd printf pushd pwd read readarray readonly return set shift
shopt source suspend test times trap true type typeset ulimit umask unalias
unset wait`. Small fallbacks, used only when no program with that name is in
the PATH: `ls` (`-a -l`), `cat`, `clear`, `mkdir` (`-p`), `env`, `which`.

**Completion of a command's own arguments** works as in bash: `complete`,
`compgen` and `compopt` with `-F -C -W -G -P -S -X -A`, the letters
`-abcdefgjksuv`, the `-o` options (`nospace`, `filenames`, `dirnames`,
`default`, `bashdefault`, `plusdirs`, `nosort`, `noquote`) and the `-D`,
`-E` and `-I` rules; the function gets `COMP_WORDS`, `COMP_CWORD`,
`COMP_LINE`, `COMP_POINT` and answers in `COMPREPLY`. So git's own
`git-completion.bash` works: put this in `~/.mmcrc` and Tab gives you
subcommands, branches and options.

```sh
source $MMC_DRIVE/PortableGit/mingw64/share/git/completion/git-completion.bash
```

`mmc --complete 'git checkout '` prints what Tab would offer for that line,
without a terminal - useful when a completion script does not behave.

```
mmc -c 'command' [$0 [$1 ...]]    mmc script.sh args    mmc -s < commands
mmc -e -x -o pipefail ...         mmc -n script.sh      #!/usr/bin/env mmc
mmc --check a.sh b.sh             syntax, and commands that are not there
mmc --complete 'git ch'           what Tab would offer for that line
```

`mmc --check` reads scripts without running them and reports syntax errors
and commands that are neither builtins, functions of the script, aliases nor
programs in the PATH (with the PATH of `/etc/profile`), with their line.
Scripts saved with Windows line ends (CR LF) are read like the others.

How it works without `fork()` (Windows has none): `( )` and `$( )` run inside
mmc on a copy of its state that is put back afterwards; in a pipeline programs
run side by side, builtins in the middle run with their output kept and fed
on, other shell code in the middle (loops, functions) runs in a child mmc
that gets the variables, functions and options; `cmd &` does the same for
shell code. Like bash, the last stage of a pipeline runs in a subshell unless
`shopt -s lastpipe`. Everything is one code path on every system.

Keys: Tab completes commands, functions, files and, where a `complete`
rule says so, a command's own arguments (git branches ...), Up/Down history,
**Ctrl-R searches the history while you type** (Ctrl-R again for the next
match further back, Ctrl-S forward, Enter runs it, Esc keeps the line for
editing, Ctrl-G drops it), Left/Right, Ctrl-Left/Right by word, Home/End,
Ctrl-A/E/K/U/W/L, Ctrl-C drops the line (and stops a running loop),
Ctrl-D leaves. An unfinished command
(`if` without `fi`, an open quote, a `\` at the end) asks for more with `> `.
Pasting several lines puts them into the line (shown as a return sign) and
runs them together when you press Enter. On Windows `.exe .com .cmd .bat` are
found without typing the extension, and scripts starting with `#!` run with
their interpreter (`#!/bin/sh` and `#!/bin/bash` use mmc when there is no
`sh` in the PATH).

History expansion works in an interactive shell (`set +H` switches it off):
`!!`, `!n`, `!-n`, `!word`, `!?word?`, the words `!^ !$ !* !!:2 !!:2-3`, the
parts `:h :t :r :e`, `:p` (only show it), `:s/old/new/` (`:gs` for every
place) and `^old^new` to run the last line again with one word changed.

Not there (yet): `coproc`, stopping a program with Ctrl-Z (Windows has no
such thing; `fg` waits for a background job), and `fc` beyond `fc -l`.
`ulimit` reports what mmc has and says so when it cannot change a limit.
These `shopt` options are accepted but do nothing: `cdspell`, `dirspell`,
`execfail`, `extdebug`, `inherit_errexit`, `localvar_inherit`,
`localvar_unset`, `globskipdots`, `cdable_vars` and the `compat*` ones;
`autocd`, `checkjobs`, `huponexit`, `histappend`, `lastpipe`, `extglob`,
`nullglob`, `dotglob`, `globstar`, `nocaseglob` and `nocasematch` do work.

## How git-bash does it

git-bash is three things: the **MSYS2 runtime** (`msys-2.0.dll`, a fork of
Cygwin that emulates POSIX on Windows), **GNU bash** compiled against it, and
the **mintty** terminal. `/` is the install folder, `/c/...` are the drives,
and paths are converted when a Windows program is started.

mmc keeps that idea but has no emulation layer: it is one native program that
calls the Win32 API on Windows and the POSIX API on Linux/macOS (see `mos.c`).

## Tested

Windows 11: builds with zero warnings (`-std=c11 -Wall -Wextra -pedantic`);
`tests/run.sh` passes all 420 bash compatibility cases, also when mmc runs
the test script itself; `mmc --check` reads the 70 shell scripts of
PortableGit, Flutter, emsdk and the Android SDK without a syntax error;
`build test` passes the 124 checks of the terminal core.
Linux, macOS and Android: compile cleanly (x86_64, aarch64, and arm for
Android), not yet run.
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
| `HackNerdFontMono-*.ttf` | Source Foundry Authors, Bitstream Inc.; icons: the Nerd Fonts project and the icon set authors | MIT and Bitstream Vera License, see `HackNerdFont-LICENSE.md`; the icon sets' licenses are listed in `HackNerdFont-README.md`; the font may travel with a program, it may not be sold on its own |
