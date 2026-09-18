# a redirection attached to a function definition applies to every call
tmp=$(mktemp -d)
log() { echo "log: $*"; } >> "$tmp/log"
log one
log two
cat "$tmp/log"
rm -rf "$tmp"
