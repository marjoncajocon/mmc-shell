# || { ...; } error handling blocks
command -v no_such_cmd_xyz >/dev/null 2>&1 || {
	echo "missing tool"
	echo "second line"
}
