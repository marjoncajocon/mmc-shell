# < reads a file on stdin
tmp=$(mktemp -d)
printf 'b\na\n' > "$tmp/in"
sort < "$tmp/in"
read -r first < "$tmp/in"; echo "$first"
rm -rf "$tmp"
