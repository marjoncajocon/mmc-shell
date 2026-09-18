# SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) and friends
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
echo "dir name: ${SCRIPT_DIR##*/}"
[ -f "$SCRIPT_DIR/$(basename "$0")" ] && echo "script found in its dir"
case $SCRIPT_DIR in /*) echo "absolute" ;; esac
echo "dirname of \$0: $(dirname "$0")"
