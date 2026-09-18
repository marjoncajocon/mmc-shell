# mmc bash-compatibility corpus

Small bash scripts with the stdout and exit status that real bash produces
for them. mmc should produce exactly the same stdout and exit status. Each
case tests one idea. The most common constructs are covered first and in
the most depth (see `RESEARCH.md` for how "common" was measured).

## Layout

```
tests/compat/
  generate.sh        regenerates every .out file with real bash
  README.md          this file
  RESEARCH.md        survey of real scripts and construct frequencies
  <category>/NAME.sh   the case script
  <category>/NAME.out  expected stdout, then "status: N"
```

Categories: `quoting`, `expansion`, `arith`, `cmdsubst`, `redirection`,
`heredoc`, `pipeline`, `lists`, `control`, `functions`, `arrays`, `cond`,
`glob`, `brace`, `builtins`, `subshell`, `options`, `realworld`.

## File format

`NAME.sh` is a self-contained bash script. Its first line is a comment
that says what the case tests.

`NAME.out` is the exact stdout of

```
cd <category>
LC_ALL=C TZ=UTC bash --norc --noprofile NAME.sh </dev/null
```

followed by one final line `status: N`, where N is the exit status. If
the script's output does not end in a newline, the `status:` line follows
the last output byte directly. Stderr is discarded and never compared.

A runner for mmc should do the same: run from the case's folder, with
`LC_ALL=C`, `TZ=UTC`, stdin from `/dev/null`, capture stdout, append
`status: $?`, and compare byte-for-byte with `NAME.out`.

## Regenerating the .out files

```
bash generate.sh                       # all cases
bash generate.sh arrays                # one category
bash generate.sh arrays/assoc_basic    # one case (.sh optional)
BASH_BIN=/usr/bin/bash bash generate.sh  # choose the bash
```

By default `generate.sh` uses `D:\env\PortableGit\usr\bin\bash.exe`
(`/d/env/PortableGit/usr/bin/bash.exe`) if it exists, otherwise `bash` from
PATH. The committed outputs were made with GNU bash 5.3.9 (PortableGit /
MSYS). Each case has a timeout (`CASE_TIMEOUT`, default 10 s). A case that
times out is reported and its `.out` is left alone.

Never edit a `.out` by hand. Change the `.sh` and regenerate.

## Rules for cases

- **Deterministic.** No dates, random values, PIDs, timings, absolute paths
  of this PC, user names or host names in the output. No network.
- **Stdout only.** Stderr is not compared, so don't depend on error message
  wording. To show that something fails, print its status (`echo $?`),
  usually from a subshell: `( cmd ) 2>/dev/null; echo "status $?"`.
- **Files.** Use only a temp dir the script makes itself
  (`tmp=$(mktemp -d)` ... `rm -rf "$tmp"`), or the case's own folder,
  read-only (for example `"$0"`).
- **Portable to Linux.** No `/c/...` paths, `cmd`, or `.exe` names.
  External commands allowed: cat, sort, grep, sed, awk, tr, cut, wc, head,
  tail, mktemp, rm, mkdir, touch, ls (simple flags only), basename,
  dirname, uname (never print its output), true, false, printf, test,
  expr, seq, xargs, tee, env. Prefer builtins.
- **No interactive input.** `read` and `select` get input from a pipe, a
  here-doc, a here-string or a file. Stdin is `/dev/null`.
- **Fast.** Each case must finish in under 2 seconds. They currently take
  0.2 to 0.5 s each under MSYS.
- **No MSYS/Linux differences.** Avoid CRLF handling, `/proc`, symlinks
  (`ln -s` copies on MSYS), execute bits (`chmod`, `[ -x file ]`),
  file names that differ only in case, and characters Windows forbids in
  file names (`\ : * ? " < > |`). Keep `set -x` output on stderr.
- **Hash order.** Associative-array keys are printed only after sorting
  (`... | sort`), because the order of `${!assoc[@]}` is an implementation
  detail.

## Notes on specific cases

- `options/set_o_query` compares the exact `set -o` / `shopt` listing
  layout (name padded, then a tab).
- `builtins/type_output` compares bash's function pretty-printing
  (`greet () { ... }`, 4-space indent).
- `arrays/declare_a` and `builtins/declare_p` compare `declare -p` output.
- `expansion/replace_ampersand` relies on bash 5.2+ `patsub_replacement`
  (on by default): `&` in a `${v/p/r}` replacement means the matched text.
- `builtins/trap_signal` sends `USR1` and `TERM` to the shell itself with
  `kill`.
