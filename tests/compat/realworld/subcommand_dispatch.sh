# dispatch "cmd_$1" functions (Makefile-like build script)
cmd_build() { echo "building ${1:-all}"; }
cmd_clean() { echo "cleaning"; }
cmd_help() { echo "commands:"; declare -F | sed -n 's/^declare -f cmd_/  /p'; }
dispatch() {
	local sub=${1:-help}
	shift || true
	if declare -F "cmd_$sub" >/dev/null; then
		"cmd_$sub" "$@"
	else
		echo "unknown command: $sub"
		return 1
	fi
}
dispatch build lib
dispatch clean
dispatch
dispatch deploy; echo "status $?"
