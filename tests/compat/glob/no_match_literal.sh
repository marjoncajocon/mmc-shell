# a pattern with no match stays literal by default
tmp=$(mktemp -d); cd "$tmp" || exit 1
echo *.none
for f in *.none; do echo "loop got [$f]"; done
cd / && rm -rf "$tmp"
