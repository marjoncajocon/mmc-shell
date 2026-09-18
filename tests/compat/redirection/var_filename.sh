# file names from variables with spaces must be quoted
tmp=$(mktemp -d)
f="$tmp/my file.txt"
echo content > "$f"
cat "$f"
ls "$tmp"
rm -rf "$tmp"
