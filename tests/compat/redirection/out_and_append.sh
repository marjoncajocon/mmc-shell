# > truncates, >> appends
tmp=$(mktemp -d)
echo one > "$tmp/f"
echo two >> "$tmp/f"
cat "$tmp/f"
echo three > "$tmp/f"
cat "$tmp/f"
rm -rf "$tmp"
