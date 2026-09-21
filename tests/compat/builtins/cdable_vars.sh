# shopt -s cdable_vars: cd NAME goes where $NAME points when NAME is no folder
tmp=$(mktemp -d); cd "$tmp" || exit 1
mkdir -p deep/place
target=$tmp/deep/place
shopt -s cdable_vars
cd target >/dev/null && [ "$PWD" = "$target" ] && echo "went to \$target"
cd "$tmp"; mkdir target; cd target && echo "a real folder wins: ${PWD##*/}"
cd / && rm -rf "$tmp"
