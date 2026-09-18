# ** with globstar recurses into directories
tmp=$(mktemp -d); cd "$tmp" || exit 1
mkdir -p a/b/c
touch top.md a/one.md a/b/two.md a/b/c/three.md a/b/c/x.txt
shopt -s globstar
echo **/*.md
echo a/**/
cd / && rm -rf "$tmp"
