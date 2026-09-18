# a while loop reading from a file redirect
tmp=$(mktemp -d)
printf 'a 1\nb 2\n' > "$tmp/f"
while read -r k v; do echo "$k=$v"; done < "$tmp/f"
rm -rf "$tmp"
