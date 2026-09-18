# tally values from input with an associative array, sorted output
declare -A seen
while read -r word; do
	[ -z "$word" ] && continue
	seen[$word]=$(( ${seen[$word]:-0} + 1 ))
done <<EOF
red
blue
red

green
red
blue
EOF
for k in "${!seen[@]}"; do printf '%s %s\n' "${seen[$k]}" "$k"; done | sort -k1,1nr -k2
