# '> file' alone creates/truncates a file
tmp=$(mktemp -d)
echo data > "$tmp/f"
> "$tmp/f"
wc -c < "$tmp/f"
: > "$tmp/g"
[ -f "$tmp/g" ] && echo created
rm -rf "$tmp"
