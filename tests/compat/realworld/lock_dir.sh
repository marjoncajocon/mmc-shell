# mkdir as an atomic lock (flutter _lock fallback)
tmp=$(mktemp -d)
lock="$tmp/upgrade.lock"
acquire() { mkdir "$lock" 2>/dev/null; }
release() { [ -n "$lock" ] && rm -rf -- "$lock"; }
acquire && echo "got lock"
acquire || echo "lock busy"
release
acquire && echo "got lock again"
release
rm -rf "$tmp"
