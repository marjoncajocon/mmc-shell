# local with several vars, no value, and -i/-a flags
f() {
	local a=1 b c=3
	local -i n=2+3
	local -a arr=(x y)
	echo "a=$a b=[$b] c=$c n=$n arr=${arr[1]} ${#arr[@]}"
	b=later
	echo "b=$b"
}
f
echo "outside: [${a-unset}] [${b-unset}]"
