# getopts with flags and option arguments
parse() {
	local OPTIND opt
	while getopts "ab:c" opt; do
		case $opt in
			a) echo "flag a" ;;
			b) echo "b=$OPTARG" ;;
			c) echo "flag c" ;;
			?) echo "bad option" ;;
		esac
	done
	shift $((OPTIND - 1))
	echo "rest: $*"
}
parse -a -b val -c file1 file2
parse -ac -bX -- -notopt
parse -z 2>/dev/null
