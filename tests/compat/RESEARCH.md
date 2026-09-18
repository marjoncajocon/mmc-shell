# What bash scripts on this PC actually use

This survey decided which constructs the compat corpus covers first and
most thoroughly. All sources were only read, never changed.

## Sources surveyed

104 script files were found. 13 of them are identical `gradlew` copies, so
90 unique files were counted: 18,491 lines, or 14,982 lines without
comment-only lines.

| Source | Unique files | Lines |
|---|---:|---:|
| Git: `mingw64/libexec/git-core` shell scripts (git-sh-setup, git-sh-i18n, git-mergetool--lib, git-mergetool, git-submodule, git-subtree, git-filter-branch, git-quiltimport, git-request-pull, git-web--browse, git-difftool--helper, git-merge-*, ...) | 15 | 5,038 |
| Git: `mingw64/libexec/git-core/mergetools/*` | 23 | 1,330 |
| Git: `mingw64/share/git/completion/git-completion.bash`, `git-prompt.sh` (sourced by the login shell through `/etc/profile.d/git-prompt.sh`) | 2 | 4,699 |
| Git: `mingw64/share/git-core/templates/hooks/*.sample` | 14 | 912 |
| PortableGit: `etc/profile`, `etc/bash.bashrc`, `etc/profile.d/*.sh` | 8 | ~560 |
| PortableGit: `usr/share/**/*.sh` | 2 | 71 |
| Flutter: `bin/flutter`, `bin/dart`, `bin/internal/*.sh` | 7 | 803 |
| Gradle: `gradle-8.14.2/bin/gradle` | 1 | 251 |
| `gradlew` under `02 gameproject` (13 found, all identical) | 1 | 160 |
| emsdk: `emsdk_env.sh` | 1 | 73 |
| Node: `npm/lib/utils/completion.sh`, node-gyp test script | 2 | 91 |
| Android NDK 21 and 28: `build/tools/*.sh`, `wrap.sh/*.sh`, `spirv-lesspipe.sh` | 13 | 4,470 |
| mmc: `D:\mmc-shell\etc\profile` | 1 | ~35 |

The user's `D:\mmc-shell\home\marjoncajocon\.mmc_history` has 258 lines of
interactive use: `ls` (89), `clear` (62), `git` (22), `exit` (18), `cd` (9),
then single uses of `echo`, `rm`, `go`, `flutter`, `gcc`, `python`, `java`.
There is no scripting in it. It shows that the interactive basics (simple
commands, `cd`, `ls`, `exit`) must be solid. The mmc `/etc/profile` uses
only `export NAME="$VAR/..."`, `export PATH="$PATH:..."` and comments.

## Method

The counts come from `grep -oE` over the non-comment lines of the 90
unique files. They are rough, since regexes are not a parser: quoted text
and heredoc bodies are counted too, and some patterns overlap. Use them to
rank constructs, not as exact figures.

## Ranked constructs (occurrences)

| # | Construct | Count |
|---:|---|---:|
| 1 | `"$var"` / `"${var}"` quoted expansion | 4295 |
| 2 | `test` / `[ ... ]` | 966 |
| 3 | `if ... then ... elif ... else ... fi` | 832 |
| 4 | positional `$1`..`$9` | 809 |
| 5 | pipe `\|` | 719 |
| 6 | function definitions `name()` / `function name` | 611 |
| 7 | `echo` | 583 |
| 8 | `$( ... )` command substitution | 549 |
| 9 | string `=` / `!=` comparisons | 471 |
| 10 | `[ -z ... ]` / `[ -n ... ]` | 396 |
| 11 | `return` | 352 |
| 12 | `local` | 342 |
| 13 | `case ... esac` | 304 |
| 14 | `&&` | 254 |
| 15 | `\|\|` | 250 |
| 16 | `exit` | 218 |
| 17 | `${v#p}` / `${v##p}` | 161 |
| 18 | `export` | 161 |
| 19 | `>&2` / `1>&2` | 151 |
| 20 | file tests `-f -d -e -s -r -w -x -h` | 149 |
| 21 | `<` input redirection | 145 |
| 22 | `${v:-x}` / `${v-x}` | 134 |
| 23 | `>/dev/null` | 121 |
| 24 | subshell `( ... )` | 113 |
| 25 | `"$@"` | 111 |
| 26 | `!` negation | 108 |
| 27 | `-eq -ne -lt -gt -le -ge` | 104 |
| 28 | `break` / `continue` | 100 |
| 29 | `for x in ...` | 99 |
| 30 | `$?` | 94 |
| 31 | backticks `` `...` `` | 94 |
| 32 | `shift` | 94 |
| 33 | `$#` | 90 |
| 34 | `while` / `until` | 90 |
| 35 | `printf` | 76 |
| 36 | `${v%p}` / `${v%%p}` | 74 |
| 37 | `eval` | 62 |
| 38 | `set` (options and `set --`) | 60 |
| 39 | `$(( ))` arithmetic | 55 |
| 40 | `[[ ... ]]` | 48 |
| 41 | `2>&1` | 47 |
| 42 | `${v:+x}` / `${v+x}` | 46 |
| 43 | `read` | 45 |
| 44 | `source` / `.` | 44 |
| 45 | `dirname` | 44 |
| 46 | arrays `a=( )`, `${a[@]}` | 39 |
| 47 | `IFS=` assignments | 35 |
| 48 | `unset` | 35 |
| 49 | `exec` | 27 |
| 50 | `$0` | 26 |
| 51 | `basename` | 25 |
| 52 | `type` | 25 |
| 53 | `$*` | 21 |
| 54 | `set -- ...` | 21 |
| 55 | `$$` | 20 |
| 56 | here-documents `<<EOF`, `<<-EOF`, `<<\EOF` | 19 |
| 57 | `${v//p/r}` / `${v/p/r}` | 17 |
| 58 | `${#v}` | 15 |
| 59 | `>>` | 14 |
| 60 | `${v:off:len}` | 13 |
| 61 | `[[ =~ ]]` | 13 |
| 62 | `compgen` / `complete` | 13 |
| 63 | `set -e` | 11 |
| 64 | `FUNCNAME` / `BASH_SOURCE` / `LINENO` | 8 |
| 65 | `${v:=x}` / `${v=x}` | 7 |
| 66 | `{ ...; }` groups | 7 |
| 67 | `trap` | 6 |
| 68 | `&` background | 6 |
| 69 | `: ${VAR:=default}` | 5 |
| 70 | `command -v` | 4 |
| 71 | `printf -v` | 4 |
| 72 | `declare` / `typeset` | 4 |
| 73 | `pushd` / `popd`, `shopt` | 3 each |
| 74 | `let`, `(( ))` command, `${v^^}`, `<( )` | 1 or 2 each |
| - | `getopts`, `mapfile`, `<<<`, `${!name}`, `select`, `readonly`, `wait`, `;&`/`;;&`, extglob | 0 locally |

External commands, in order: `sed` 40, `rm` 39, `cat` 39, `mkdir` 26,
`expr` 26, `tr` 22, `grep` 20, `uname` 12, `sort` 10, `awk` 10, `ls` 9,
`cut` 9, `wc` 6, `touch` 4, `mktemp` 4, `xargs` 2, `env` 2.

The zero-count constructs are still in the corpus. They are common in
scripts outside this PC: CI scripts, user bashrc files, installers and
getopts-based tools. They get fewer cases than the top of the table.

### Observations

- The local scripts are mostly POSIX-sh style: git's scripts, gradlew and
  the NDK tools are written for `/bin/sh`. That puts `[ ]`, `case`,
  `$( )`, `${v#p}`/`${v%p}`, `${v:-x}`/`${v:+x}`, `eval`, `set --` and
  backticks far above bashisms. Flutter, git-completion.bash, git-prompt.sh
  and npm's completion.sh are the bash-specific ones: `[[ ]]`, `=~`,
  `local`, arrays, `printf -v`, `compgen`, `$'...'`, `<( )`.
- `echo ... >&2` for error messages is everywhere, and so is
  `cmd >/dev/null 2>&1`. Stderr routing must be right even though the
  corpus never compares stderr.
- Functions with `local` and `return` are the main structuring tool. In
  git-completion.bash almost every function starts with a block of
  `local` declarations.

## Tricky or unusual constructs found in real scripts

| Construct | Where | Corpus case |
|---|---|---|
| `pat=$"${pat//\//\\/}"`, replacing `/` inside `${ }` with escaped slashes, inside `$"..."` | NDK `prebuilt-common.sh` | `expansion/replace_slash`, `quoting/locale_dollar_quote` |
| `printf -v gitstring -- "$fmt" ...`, `printf -v "name_${section}" %s ...` (dynamic target name) | `git-prompt.sh`, `git-completion.bash` | `builtins/printf_v`, `realworld/git_prompt_style` |
| `IFS=$'\r\n' read -r "$2" <"$1"` (`__git_eread`: read into a variable named by a parameter) | `git-prompt.sh` | `builtins/read_named_var_indirect` |
| `if ! IFS=$'\n' COMPREPLY=($(...)); then` (assignment prefix applied to an array assignment, status of the substitution) | npm `completion.sh` | `realworld/npm_completion_style` |
| `done < <(__git ls-tree -z ...)` (process substitution feeding a loop) | `git-completion.bash` | `expansion/process_subst_basic`, `arrays/mapfile` |
| `"${rest:$((${#word}-$len))}"` (arithmetic inside a substring offset) | `git-completion.bash` | `expansion/substring`, `expansion/arith_in_var_index` |
| `for ((i=$__git_cmd_idx; i < ${#words[@]}; i++))` | `git-completion.bash` | `arith/c_for_loop` |
| `unset $(compgen -v __gitcomp_builtin_)`, `compgen -W` | `git-completion.bash` | `builtins/compgen_w`, `expansion/indirect_prefix` |
| `JVM_OPTS[${#JVM_OPTS[*]}]="..."` then `exec "$JAVACMD" "${JVM_OPTS[@]}" ... "$@"` | every `gradlew` | `arrays/gradle_append_idiom` |
| `APP_HOME=$( cd -P "${APP_HOME:-./}.." > /dev/null && printf '%s\n' "$PWD" ) \|\| exit` | `gradlew`, `gradle` | `realworld/gradle_app_home` |
| `for arg do ... shift; set -- "$@" "$arg"; done`, `if case $arg in ...) true;; esac; then` | `gradlew` | `realworld/gradle_arg_rewrite` |
| `eval "set -- $( printf '%s\n' "$DEFAULT_JVM_OPTS ..." \| xargs -n1 \| sed ... )"` | `gradlew`, `gradle` | `builtins/eval_set_quoted` |
| `warn () { echo "$*"; } >&2` (redirection attached to a function definition) | `gradlew` | `functions/redirect_on_definition`, `realworld/gradle_warn_die` |
| `case $n in (0) set -- ;; (1) set -- "$args0" ;; ...` (leading `(` in case patterns) | `gradle` | `control/case_paren_style` |
| `[[ $OS =~ MINGW.* \|\| $OS =~ CYGWIN.* \|\| $OS =~ MSYS.* ]]` | `flutter`, `dart` | `cond/regex_alternation_or` |
| `[[ "$(git --version)" == *"Apple Git"* ]]` | flutter `shared.sh` | `cond/dbl_bracket_contains` |
| `trap _rmlock INT TERM EXIT`, `trap 'rm -f "$es_tmp"' EXIT`, `trap - EXIT`, `trap '...' 0` | flutter, git-filter-branch | `builtins/trap_*`, `realworld/trap_tmp_cleanup` |
| `(cd "$DIR" && cmd >&2) && break` in a retry `while [[ "$n" -gt 0 ]]` loop | flutter `shared.sh` | `realworld/retry_loop` |
| `>&2 echo "..."` (redirection before the command word) | flutter `shared.sh` | `redirection/to_stderr`, `redirection/prefix_position` |
| `function name {` and `function name () {` | flutter | `functions/define_forms` |
| `command -v curl > /dev/null 2>&1 \|\| { ...; }` | flutter `update_dart_sdk.sh` | `realworld/command_exists`, `lists/group_with_or` |
| `case "$(uname -s)" in`, `case "$(uname -m)" in` | flutter, NDK | `realworld/uname_case`, `realworld/uname_arch` |
| `"${ENGINE_REALM:+/$ENGINE_REALM}"`, `"$MSYS2_PATH${ORIGINAL_PATH:+:${ORIGINAL_PATH}}"` | flutter, `/etc/profile` | `expansion/alt_with_colon_join`, `realworld/profile_path_setup` |
| `: "${GIT_OBJECT_DIRECTORY="$(git rev-parse ...)"}"`, `: ${QUILT_PATCHES:=patches}` | git-sh-setup, git-quiltimport | `expansion/colon_noop_default` |
| `${branch:+"$branch"}` used to add an optional quoted argument | git-submodule | `expansion/alternate` |
| `case "$(declare -p PS1 2>/dev/null)" in 'declare -x '*)` | `bash.bashrc` | `builtins/declare_p` |
| `shopt -q login_shell \|\| . /etc/profile.d/git-prompt.sh` | `bash.bashrc` | `builtins/shopt_toggles`, `builtins/source_file` |
| `exec 1>&2` (redirect the rest of the script) | `pre-commit.sample` | `redirection/exec_fd_save_restore` |
| `while read local_ref local_oid remote_ref remote_oid` | `pre-push.sample` | `realworld/git_hook_pre_push` |
| `case "$COMMIT_SOURCE,$SHA1" in` with patterns such as `,*)` (in a commented-out example) | `prepare-commit-msg.sample` | `control/case_comma_join` |
| `while read patch_name level garbage <&3 ... done 3<"$series"` | git-quiltimport | `redirection/exec_read_fd` |
| `while IFS='' read -r header_line && test -n "$header_line"` | git-filter-branch | `builtins/read_ifs_empty` |
| `IFS='\n'` / `IFS=' <TAB><NL>'` set literally across lines, `oldIFS=$IFS; IFS=#; ...; IFS=$oldIFS` | git-mergetool--lib, mergetools/vimdiff | `quoting/ifs_*`, `functions/local_ifs` |
| `for c in $(substring ... \| sed 's:.:&#:g')` with `IFS=#` | mergetools/vimdiff | `quoting/ifs_custom` |
| `printf "%$(($indent * 2))s%s\n" '' "$*"` (computed width) | git-subtree | `builtins/printf_width` |
| `set -- $(git rerere remaining)`, `set -- $files` (deliberate splitting) | git-mergetool | `builtins/set_split_var` |
| `eval "$GIT_EDITOR" '"$@"'` | git-sh-setup | `builtins/eval_basic` |
| `functions=$(cat << \EOF ... EOF )` (here-doc inside `$( )`) | git-mergetool--lib | `cmdsubst/heredoc_inside` |
| `cat >&2 <<-EOF` (tab-stripped heredoc to stderr) | git-sh-setup | `heredoc/tab_strip` |
| `CURRENT_SCRIPT="$BASH_SOURCE"`, `${BASH_SOURCE-}` | `emsdk_env.sh` | `realworld/emsdk_source_detect`, `builtins/source_bash_source` |
| `[ $# -eq 0 ] && set -- "-"`, `[ "$*" != "-" ] && set -- -- "$@"` | PortableGit `usr/share/vim/vim92/macros/less.sh` | `lists/test_and_default` |
| `test -z "${LC_ALL:-${LC_CTYPE:-$LANG}}" && export LANG=$(...)` | `profile.d/lang.sh` | `expansion/nested_default` |

Constructs not seen locally but covered because they are common in
real-world bash elsewhere: `set -euo pipefail`, `IFS=: read -ra`,
`read -d ''`, `mapfile -t`, here-strings `<<<`, `declare -A`,
`local -a/-A`, `"${@:2}"`, `${!name}`, `${v,,}`/`${v^^}`, extglob,
`shopt -s nullglob/globstar/dotglob`, `getopts`, `select`,
`exec {fd}>`, `wait $!`, `PIPESTATUS`, `lastpipe`, `$'...'`.

## Top 20 by frequency

1. `"$var"` quoted expansion
2. `[ ... ]` / `test`
3. `if` / `elif` / `else`
4. `$1`..`$9`
5. pipelines
6. function definitions
7. `echo`
8. `$( )`
9. `=` / `!=` string comparisons
10. `-z` / `-n`
11. `return`
12. `local`
13. `case`
14. `&&`
15. `||`
16. `exit`
17. `${v#p}` / `${v##p}`
18. `export`
19. `>&2`
20. file tests (`-f -d -e ...`)
