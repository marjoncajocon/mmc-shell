# {var}> allocates a free fd number into var
tmp=$(mktemp -d)
exec {fd}> "$tmp/o"
echo via-named >&$fd
[ "$fd" -ge 10 ] && echo "fd>=10"
exec {fd}>&-
cat "$tmp/o"
rm -rf "$tmp"
