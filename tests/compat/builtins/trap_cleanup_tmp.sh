# trap 'rm -f "$tmp"' EXIT removes a temp file
dir=$(mktemp -d)
(
	tmpf="$dir/work.tmp"
	trap 'rm -f "$tmpf"' EXIT
	echo data > "$tmpf"
	[ -f "$tmpf" ] && echo "exists during"
)
[ -f "$dir/work.tmp" ] || echo "removed after"
rm -rf "$dir"
