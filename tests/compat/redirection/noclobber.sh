# set -C refuses to overwrite; >| forces it
tmp=$(mktemp -d)
echo a > "$tmp/f"
set -C
( echo b > "$tmp/f" ) 2>/dev/null; echo "status $?"
echo c >| "$tmp/f"
cat "$tmp/f"
set +C
rm -rf "$tmp"
