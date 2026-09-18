# a strict-mode script with helper functions and a main
set -euo pipefail
log() { printf '[%s] %s\n' "$1" "${*:2}"; }
cleanup() { log INFO "cleanup"; }
trap cleanup EXIT
main() {
	local name=${1:-world}
	log INFO "hello $name"
	local count
	count=$(printf 'a\nb\nc\n' | grep -c .)
	log INFO "count=$count"
	if ! grep -q x <<< "abc"; then log WARN "no x"; fi
}
main "$@"
main tester
