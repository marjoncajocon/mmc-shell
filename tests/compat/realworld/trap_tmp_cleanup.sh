# mktemp + trap 'rm -rf "$tmp"' EXIT inside a script run in a subshell
check=$(mktemp -d)
(
	set -e
	work=$(mktemp -d "$check/work.XXXXXX")
	trap 'rm -rf "$work"' EXIT
	echo "payload" > "$work/file"
	cat "$work/file"
	false
	echo "not reached"
)
echo "subshell status $?"
ls "$check" | wc -l
rm -rf "$check"
