# pushd/popd directory stack (paths not printed)
tmp=$(mktemp -d)
mkdir -p "$tmp/a/b"
cd "$tmp" || exit 1
pushd a >/dev/null
echo "${PWD##*/}"
pushd b >/dev/null
echo "${PWD##*/} depth ${#DIRSTACK[@]}"
popd >/dev/null
echo "${PWD##*/}"
popd >/dev/null
[ "$PWD" = "$tmp" ] && echo "back to start"
popd 2>/dev/null; echo "empty stack status $?"
cd / && rm -rf "$tmp"
