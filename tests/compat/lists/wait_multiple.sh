# wait for several background jobs
tmp=$(mktemp -d)
for i in 1 2 3; do ( echo "job$i" > "$tmp/$i" ) & done
wait
cat "$tmp"/1 "$tmp"/2 "$tmp"/3
rm -rf "$tmp"
