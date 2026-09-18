# recursion with local variables and a global accumulator
out=
walk() {
	local depth=$1
	out="$out$depth"
	[ "$depth" -lt 4 ] && walk $((depth + 1))
	out="$out-$depth"
}
walk 1
echo "$out"
