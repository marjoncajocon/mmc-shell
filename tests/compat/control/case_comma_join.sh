# case on joined values "$a,$b" (prepare-commit-msg hook style)
check() {
	case "$1,$2" in
		merge,) echo "merge" ;;
		message,*) echo "message" ;;
		,|*,) echo "empty second" ;;
		*) echo "other $1 $2" ;;
	esac
}
check merge ""
check message x
check "" ""
check commit HEAD
