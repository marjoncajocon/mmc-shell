# cd in a subshell does not change the parent directory
tmp=$(mktemp -d)
mkdir "$tmp/inner"
cd "$tmp" || exit 1
( cd inner && echo "in: ${PWD##*/}" )
echo "out: ${PWD##*/}" | sed 's/out: .*/out: tmpdir/'
[ "$PWD" = "$tmp" ] && echo unchanged
cd / && rm -rf "$tmp"
