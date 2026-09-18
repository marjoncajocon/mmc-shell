# generate a script with a heredoc and run it with bash (mixed quoting)
tmp=$(mktemp -d)
greeting="Hi"
cat > "$tmp/gen.sh" <<EOF
#!/bin/sh
name=\${1:-anon}
echo "$greeting, \$name"
EOF
. "$tmp/gen.sh"
set -- bob
. "$tmp/gen.sh"
rm -rf "$tmp"
