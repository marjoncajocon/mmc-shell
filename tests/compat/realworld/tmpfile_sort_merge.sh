# temp files, sort, and join-like merging with a loop
tmp=$(mktemp -d)
printf '3 c\n1 a\n2 b\n' > "$tmp/in"
sort -n "$tmp/in" > "$tmp/sorted"
while read -r num letter; do
	printf '%s%s ' "$letter" "$num"
done < "$tmp/sorted"
echo
sort -k2 -r "$tmp/in" | cut -d' ' -f1 | tr '\n' ','
echo
rm -rf "$tmp"
