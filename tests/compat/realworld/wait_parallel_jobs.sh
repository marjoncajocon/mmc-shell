# run jobs in the background, collect statuses in order
tmp=$(mktemp -d)
pids=()
for n in 1 2 3; do
	( echo "result $n" > "$tmp/$n"; exit $((n % 2)) ) &
	pids+=($!)
done
i=1
for p in "${pids[@]}"; do
	wait "$p"; echo "job $i status $? -> $(cat "$tmp/$i")"
	i=$((i + 1))
done
rm -rf "$tmp"
