# redirecting a { } group and a loop
tmp=$(mktemp -d)
{ echo x; echo y; } > "$tmp/g"
for i in 1 2 3; do echo "$i"; done > "$tmp/l"
cat "$tmp/g" "$tmp/l"
rm -rf "$tmp"
