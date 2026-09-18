# gradlew style warn/die helpers and max-fd check
warn() { echo "$*"; } >&2
die() {
	echo
	echo "$*"
	echo
	exit 1
} >&2
MAX_FD=maximum
case $MAX_FD in
	max*) MAX_FD=4096 ;;
esac
echo "MAX_FD=$MAX_FD"
warn "a warning"
( die "fatal problem" ); echo "die status $?"
