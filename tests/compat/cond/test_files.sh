# file tests -e -f -d -s -r -w on a temp dir
tmp=$(mktemp -d)
touch "$tmp/empty"
echo data > "$tmp/full"
mkdir "$tmp/dir"
for p in empty full dir missing; do
	r=
	[ -e "$tmp/$p" ] && r="$r e"
	[ -f "$tmp/$p" ] && r="$r f"
	[ -d "$tmp/$p" ] && r="$r d"
	[ -s "$tmp/$p" ] && r="$r s"
	[ -r "$tmp/$p" ] && r="$r r"
	echo "$p:$r"
done
rm -rf "$tmp"
