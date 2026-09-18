# word counting with an associative array
declare -A count
for w in the cat the dog the end cat; do
	count[$w]=$(( ${count[$w]:-0} + 1 ))
done
for k in "${!count[@]}"; do echo "$k ${count[$k]}"; done | sort
