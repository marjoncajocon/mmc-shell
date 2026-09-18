# cd, cd -, PWD/OLDPWD and cd to missing dir
tmp=$(mktemp -d)
mkdir "$tmp/x"
cd "$tmp/x" || exit 1
echo "${PWD##*/}"
cd ..
[ "$PWD" = "$tmp" ] && echo "up ok"
cd - >/dev/null
echo "${PWD##*/} old=${OLDPWD##*/}" | sed "s/old=.*/old=tmp/"
cd "$tmp/nope" 2>/dev/null; echo "status $?"
[ "$(pwd)" = "$PWD" ] && echo "pwd matches"
cd / && rm -rf "$tmp"
