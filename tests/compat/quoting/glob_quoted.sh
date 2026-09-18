# quoted glob characters are not expanded
tmp=$(mktemp -d); cd "$tmp" || exit 1
touch a.txt b.txt
echo "*.txt" '*.txt' \*.txt
echo *.txt
cd / && rm -rf "$tmp"
