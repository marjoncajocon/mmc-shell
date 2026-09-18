# glob with a directory prefix and in variables
tmp=$(mktemp -d)
mkdir "$tmp/d"; touch "$tmp/d/x.1" "$tmp/d/y.1"
for f in "$tmp"/d/*.1; do basename "$f"; done
pat="$tmp/d/*.1"
n=0; for f in $pat; do n=$((n+1)); done; echo "$n from unquoted var"
for f in "$pat"; do echo "quoted var: $(basename "$f")"; done
rm -rf "$tmp"
