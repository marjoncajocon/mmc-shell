# shopt -s nullglob removes non-matching patterns
tmp=$(mktemp -d); cd "$tmp" || exit 1
shopt -s nullglob
for f in *.none; do echo "never $f"; done
files=(*.none)
echo "${#files[@]}"
touch x.sh
files=(*.sh)
echo "${#files[@]} ${files[0]}"
cd / && rm -rf "$tmp"
