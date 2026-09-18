# globs do not expand in assignments or [[ ]] but do in arrays
tmp=$(mktemp -d); cd "$tmp" || exit 1
touch one two
x=*
echo "$x"
arr=(*)
echo "${#arr[@]}"
[[ * == \* ]] && echo "no glob in [[ ]]"
cd / && rm -rf "$tmp"
