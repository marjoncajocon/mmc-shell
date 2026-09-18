# open a file for reading on fd 3 and read line by line
tmp=$(mktemp -d)
printf 'one\ntwo\nthree\n' > "$tmp/f"
exec 3< "$tmp/f"
read -r a <&3
read -r b <&3
exec 3<&-
echo "$a $b"
while read -r p <&3; do echo "$p"; done 3< "$tmp/f"
rm -rf "$tmp"
