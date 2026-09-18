# die() / usage() helpers with stderr and exit codes
usage() { echo "usage: ${0##*/} <cmd>"; }
die() { echo "error: $*" >&2; exit 1; }
run() {
	[ $# -ge 1 ] || { usage; return 64; }
	case $1 in
		ok) echo "ran ok" ;;
		*) ( die "bad command $1" ) ;;
	esac
}
run; echo "status $?"
run ok; echo "status $?"
run nope; echo "status $?"
