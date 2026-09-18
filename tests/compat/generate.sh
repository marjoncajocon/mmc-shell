#!/usr/bin/env bash
# generate.sh - (re)generate the expected .out files of the compat corpus
# by running every case with REAL bash.
#
# usage:  bash generate.sh [case-or-category ...]
#   bash generate.sh                     all cases
#   bash generate.sh arrays              one category
#   bash generate.sh arrays/assoc_basic  one case (with or without .sh)
#
# env:    BASH_BIN   bash to use (default: PortableGit bash if present, else bash)
#         CASE_TIMEOUT  seconds per case (default 10)
#
# Each case runs from its own folder as:
#   LC_ALL=C TZ=UTC $BASH_BIN --norc --noprofile NAME.sh </dev/null
# stdout goes to NAME.out, followed by one line "status: N".
# A case that exceeds the timeout is reported and gets no new .out.

here=$(cd "$(dirname "$0")" && pwd)

if [ -z "$BASH_BIN" ]; then
	if [ -x /d/env/PortableGit/usr/bin/bash.exe ]; then
		BASH_BIN=/d/env/PortableGit/usr/bin/bash.exe
	else
		BASH_BIN=bash
	fi
fi
CASE_TIMEOUT=${CASE_TIMEOUT:-10}

have_timeout=no
command -v timeout >/dev/null 2>&1 && have_timeout=yes

list_cases() {
	if [ $# -eq 0 ]; then
		set -- "$here"/*/
	fi
	for a in "$@"; do
		a=${a%/}
		a=${a%.sh}
		case $a in /*) p=$a ;; *) p=$here/$a ;; esac
		if [ -d "$p" ]; then
			for f in "$p"/*.sh; do
				[ -f "$f" ] && printf '%s\n' "$f"
			done
		elif [ -f "$p.sh" ]; then
			printf '%s\n' "$p.sh"
		else
			echo "generate.sh: no such case or category: $a" >&2
		fi
	done
}

n=0 bad=0
while IFS= read -r f; do
	dir=${f%/*}
	name=${f##*/}
	name=${name%.sh}
	(
		cd "$dir" || exit 1
		if [ $have_timeout = yes ]; then
			LC_ALL=C TZ=UTC timeout "$CASE_TIMEOUT" "$BASH_BIN" --norc --noprofile "$name.sh" </dev/null >"$name.out.tmp" 2>/dev/null
		else
			LC_ALL=C TZ=UTC "$BASH_BIN" --norc --noprofile "$name.sh" </dev/null >"$name.out.tmp" 2>/dev/null
		fi
		st=$?
		if [ $have_timeout = yes ] && [ $st -eq 124 ]; then
			rm -f "$name.out.tmp"
			exit 124
		fi
		echo "status: $st" >>"$name.out.tmp"
		mv -f "$name.out.tmp" "$name.out"
	)
	if [ $? -eq 124 ]; then
		echo "TIMEOUT: ${dir##*/}/$name" >&2
		bad=$((bad + 1))
	fi
	n=$((n + 1))
done < <(list_cases "$@")

echo "generated $n cases ($bad timeouts) with $BASH_BIN"
