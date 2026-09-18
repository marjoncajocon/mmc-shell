# <> opens a file read-write
tmp=$(mktemp -d)
echo hello > "$tmp/f"
exec 5<> "$tmp/f"
read -r l <&5
exec 5>&-
echo "$l"
rm -rf "$tmp"
