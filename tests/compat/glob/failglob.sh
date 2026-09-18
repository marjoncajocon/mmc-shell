# shopt -s failglob makes a non-matching pattern an error
tmp=$(mktemp -d); cd "$tmp" || exit 1
shopt -s failglob
( echo *.none ) 2>/dev/null
echo "status $?"
cd / && rm -rf "$tmp"
