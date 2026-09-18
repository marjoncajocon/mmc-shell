# $(< file) reads a file without cat
tmp=$(mktemp -d)
printf 'line1\nline2\n' > "$tmp/f"
x=$(< "$tmp/f")
echo "$x"
rm -rf "$tmp"
