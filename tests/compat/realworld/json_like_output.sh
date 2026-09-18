# building simple JSON with printf and a loop
items=("alpha" "be\"ta" "gamma")
out='['
sep=
for it in "${items[@]}"; do
	esc=${it//\\/\\\\}
	esc=${esc//\"/\\\"}
	out+="$sep\"$esc\""
	sep=,
done
out+=']'
echo "$out"
printf '{"count": %d, "first": "%s"}\n' "${#items[@]}" "${items[0]}"
