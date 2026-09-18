# set -e is ignored inside a function called from a condition
set -e
f() { false; echo "after false in f"; }
if f; then echo "f ok"; fi
f || true
echo done
