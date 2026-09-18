# | alternatives and bracket patterns in case
for x in -h --help -v 7 abc ""; do
	case "$x" in
		-h|--help) echo "help" ;;
		-v|--verbose) echo "verbose" ;;
		[0-9]) echo "digit" ;;
		"") echo "empty" ;;
		*) echo "other" ;;
	esac
done
