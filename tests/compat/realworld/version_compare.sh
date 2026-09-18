# compare dotted version strings (e.g. require git >= 2.30)
version_ge() {
	local IFS=.
	local -a a=($1) b=($2)
	local i
	for ((i = 0; i < 3; i++)); do
		local x=${a[i]:-0} y=${b[i]:-0}
		((10#$x > 10#$y)) && return 0
		((10#$x < 10#$y)) && return 1
	done
	return 0
}
for pair in "2.45.1 2.30" "2.9 2.10" "1.0.0 1.0" "3.0.10 3.0.9" "1.08 1.8"; do
	set -- $pair
	if version_ge "$1" "$2"; then echo "$1 >= $2"; else echo "$1 < $2"; fi
done
