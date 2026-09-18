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
build                 Windows: mmc.exe and mmc-shell.exe
build cross           every platform, into dist\
build install D:\mmc  copy the programs into a folder
build clean
```

```
make                  Linux / macOS (make CC=gcc works too: it is plain C11)
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

## Install on Windows

1. `build install D:\mmc` (any folder on any drive works).
2. Add that folder to the Windows `PATH`
   (Settings → "Edit environment variables for your account" → Path → New).
3. Type `mmc-shell` in any terminal, in Win+R, or in the address bar of an
   Explorer window — the shell opens **in that folder**, like git-bash does.

> **Why `mmc-shell` and not `mmc`?** Windows already has its own `mmc.exe`
> (Microsoft Management Console) in `C:\Windows\System32`, and System32 comes
> first in the PATH, so typing `mmc` outside the shell starts that one.
> `mmc.exe` and `mmc-shell.exe` are the same program. *Inside* the shell `mmc`
> always means this shell. On Linux and macOS there is no collision.

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
