# & runs in background; wait collects; output order made deterministic
tmp=$(mktemp -d)
( echo bg > "$tmp/o" ) &
wait
cat "$tmp/o"
(exit 5) &
wait $!
echo "wait status $?"
rm -rf "$tmp"
