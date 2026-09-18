# recursive function using echo/$( )
fact() {
	if [ "$1" -le 1 ]; then echo 1; return; fi
	local prev=$(fact $(($1 - 1)))
	echo $(($1 * prev))
}
fact 10
