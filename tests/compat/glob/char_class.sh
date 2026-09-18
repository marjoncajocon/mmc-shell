# POSIX classes inside brackets
tmp=$(mktemp -d); cd "$tmp" || exit 1
touch A1 b2 _c 9d
echo [[:alpha:]]*
echo [[:digit:]]*
echo [[:alnum:]_]?
cd / && rm -rf "$tmp"
