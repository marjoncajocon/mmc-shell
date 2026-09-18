# parse a key=value config file with comments and blank lines
tmp=$(mktemp -d)
cat > "$tmp/app.conf" <<'EOF'
# settings
name = demo app
port=8080

debug = true
path="/opt/app"
EOF
while IFS='=' read -r key value; do
	key=${key//[[:space:]]/}
	[ -z "$key" ] && continue
	case $key in \#*) continue ;; esac
	value=${value#"${value%%[![:space:]]*}"}
	value=${value%"${value##*[![:space:]]}"}
	value=${value%\"}; value=${value#\"}
	printf '%s -> [%s]\n' "$key" "$value"
done < "$tmp/app.conf"
rm -rf "$tmp"
