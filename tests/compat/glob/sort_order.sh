# glob results are sorted (C locale byte order)
tmp=$(mktemp -d); cd "$tmp" || exit 1
touch 10 9 1 _x B a
echo *
cd / && rm -rf "$tmp"
