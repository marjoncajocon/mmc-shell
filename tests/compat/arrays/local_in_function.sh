# local -a and local -A arrays in functions
f() {
	local -a list=(1 2 3)
	local -A map=([k]=v)
	echo "${#list[@]} ${map[k]}"
}
f
echo "[${list[*]}] [${map[k]}]"
