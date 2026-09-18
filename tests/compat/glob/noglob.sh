# set -f disables pathname expansion
tmp=$(mktemp -d); cd "$tmp" || exit 1
touch a b
set -f
echo *
set +f
echo *
cd / && rm -rf "$tmp"
