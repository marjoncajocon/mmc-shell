# tee duplicates output to a file
tmp=$(mktemp -d)
echo data | tee "$tmp/copy" | tr a-z A-Z
cat "$tmp/copy"
rm -rf "$tmp"
