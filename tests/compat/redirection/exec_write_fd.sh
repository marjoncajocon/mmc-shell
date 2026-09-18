# write to a custom fd opened for writing
tmp=$(mktemp -d)
exec 4> "$tmp/out"
echo first >&4
echo second >&4
exec 4>&-
cat "$tmp/out"
rm -rf "$tmp"
