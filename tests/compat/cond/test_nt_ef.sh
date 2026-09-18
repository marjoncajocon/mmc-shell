# -ef compares files; -nt with a missing file
tmp=$(mktemp -d)
touch "$tmp/a"
[ "$tmp/a" -ef "$tmp/./a" ] && echo "same file"
[ "$tmp/a" -nt "$tmp/missing" ] && echo "newer than missing"
rm -rf "$tmp"
