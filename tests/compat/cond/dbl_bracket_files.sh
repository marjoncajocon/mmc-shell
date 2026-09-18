# file tests in [[ ]] with a directory walk
tmp=$(mktemp -d)
mkdir "$tmp/sub"; touch "$tmp/sub/f"
[[ -d $tmp/sub && -f $tmp/sub/f ]] && echo both
[[ -e $tmp/none ]] || echo "none missing"
rm -rf "$tmp"
