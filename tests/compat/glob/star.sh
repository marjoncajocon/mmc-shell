# * matches files, sorted
tmp=$(mktemp -d); cd "$tmp" || exit 1
touch b.txt a.txt c.log .hidden
echo *
echo *.txt
cd / && rm -rf "$tmp"
