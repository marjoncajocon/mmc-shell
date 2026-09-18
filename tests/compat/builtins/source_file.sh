# source / . runs a file in the current shell, with arguments
tmp=$(mktemp -d)
cat > "$tmp/lib.sh" <<'EOF'
LIB_VAR=loaded
lib_fn() { echo "lib_fn: $*"; }
echo "sourced with $# args: $*"
EOF
. "$tmp/lib.sh"
echo "$LIB_VAR"
lib_fn x
source "$tmp/lib.sh" p q
rm -rf "$tmp"
