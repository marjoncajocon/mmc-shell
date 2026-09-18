# leading : in optstring: silent errors, ? and : cases
set -- -v -x -f
OPTIND=1
while getopts ":f:v" opt; do
	case $opt in
		v) echo verbose ;;
		f) echo "file=$OPTARG" ;;
		\?) echo "unknown -$OPTARG" ;;
		:) echo "-$OPTARG needs a value" ;;
	esac
done
