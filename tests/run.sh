#!/usr/bin/env bash
# run.sh - runs the compat corpus (tests/compat) through mmc and compares
# stdout and exit status with what real bash gave (the .out files).
#
# usage:  bash tests/run.sh [-v] [-q] [case-or-category ...]
#   -v   show the difference for every failing case
#   -q   only the summary
# env:    MMC           the mmc program (default: the one in the repo root)
#         CASE_TIMEOUT  seconds per case (default 10)
#
# Every case runs like generate.sh runs bash:
#   cd <category>; LC_ALL=C TZ=UTC $MMC --norc --noprofile NAME.sh </dev/null
# The names of failing cases go to tests/failed.txt.

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
corpus=$here/compat

if [ -z "$MMC" ]; then
	if [ -x "$root/mmc.exe" ]; then MMC=$root/mmc.exe; else MMC=$root/mmc; fi
fi
CASE_TIMEOUT=${CASE_TIMEOUT:-10}
verbose=no quiet=no
while [ $# -gt 0 ]; do
	case $1 in
		-v) verbose=yes ;;
		-q) quiet=yes ;;
		*) break ;;
	esac
	shift
done

have_timeout=no
command -v timeout >/dev/null 2>&1 && have_timeout=yes

list_cases() {
	if [ $# -eq 0 ]; then
		set -- "$corpus"/*/
	fi
	for a in "$@"; do
		a=${a%/}
		a=${a%.sh}
		case $a in /*) p=$a ;; *) p=$corpus/$a ;; esac
		if [ -d "$p" ]; then
			for f in "$p"/*.sh; do
				[ -f "$f" ] && printf '%s\n' "$f"
			done
		elif [ -f "$p.sh" ]; then
			printf '%s\n' "$p.sh"
		fi
	done
}

pass=0 fail=0
: >"$here/failed.txt"
tmp=${TMPDIR:-/tmp}/mmc-run.$$
while IFS= read -r f; do
	dir=${f%/*}
	name=${f##*/}
	name=${name%.sh}
	cat=${dir##*/}
	(
		cd "$dir" || exit 1
		if [ $have_timeout = yes ]; then
			LC_ALL=C TZ=UTC timeout "$CASE_TIMEOUT" "$MMC" --norc --noprofile "$name.sh" </dev/null >"$tmp" 2>/dev/null
		else
			LC_ALL=C TZ=UTC "$MMC" --norc --noprofile "$name.sh" </dev/null >"$tmp" 2>/dev/null
		fi
		echo "status: $?" >>"$tmp"
	)
	if cmp -s "$tmp" "$dir/$name.out"; then
		pass=$((pass + 1))
		[ $quiet = no ] && [ $verbose = yes ] && echo "PASS $cat/$name"
	else
		fail=$((fail + 1))
		echo "$cat/$name" >>"$here/failed.txt"
		if [ $quiet = no ]; then
			echo "FAIL $cat/$name"
			if [ $verbose = yes ]; then
				diff "$dir/$name.out" "$tmp" | head -20 | sed 's/^/    /'
			fi
		fi
	fi
done < <(list_cases "$@")
rm -f "$tmp"
total=$((pass + fail))
echo "$pass of $total passed, $fail failed (list: tests/failed.txt)"
[ $fail -eq 0 ]
