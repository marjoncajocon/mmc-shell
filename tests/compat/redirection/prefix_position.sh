# a redirection may appear before or between words
tmp=$(mktemp -d)
> "$tmp/f" echo first
echo a >> "$tmp/f" b
cat "$tmp/f"
rm -rf "$tmp"
