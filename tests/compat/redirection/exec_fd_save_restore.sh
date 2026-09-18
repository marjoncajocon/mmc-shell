# exec 3>&1 saves stdout; exec >file redirects the rest of the script
tmp=$(mktemp -d)
exec 3>&1
exec > "$tmp/log"
echo "into log"
exec 1>&3 3>&-
echo "back on stdout"
cat "$tmp/log"
rm -rf "$tmp"
