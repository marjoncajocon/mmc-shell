# ? and [...] and [!...] patterns
tmp=$(mktemp -d); cd "$tmp" || exit 1
touch a1 a2 b1 ab
echo a?
echo [ab]1
echo a[!0-9]
echo a[0-9]
cd / && rm -rf "$tmp"
