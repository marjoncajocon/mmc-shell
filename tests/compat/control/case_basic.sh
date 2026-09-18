# case with literal and wildcard patterns
for x in start stop foo; do
	case $x in
		start) echo "starting" ;;
		stop) echo "stopping" ;;
		*) echo "unknown: $x" ;;
	esac
done
