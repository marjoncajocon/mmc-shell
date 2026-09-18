# write a file with cat > file <<EOF (config generation)
tmp=$(mktemp -d)
ver=1.2
cat > "$tmp/conf" <<EOF
version=$ver
name="app"
EOF
cat "$tmp/conf"
rm -rf "$tmp"
