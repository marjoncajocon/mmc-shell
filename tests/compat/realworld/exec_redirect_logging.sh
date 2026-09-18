# exec 3>&1 and { ...; } 2>&1 | ... style logging of a block
tmp=$(mktemp -d)
exec 3>&1
{
	echo "stdout line"
	echo "stderr line" >&2
	echo "to saved fd" >&3
} > "$tmp/log" 2>&1
exec 3>&-
sort "$tmp/log"
{ echo "b-out"; echo "a-err" >&2; } 2>&1 | sort | sed 's/^/| /'
rm -rf "$tmp"
