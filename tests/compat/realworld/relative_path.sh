# compute a relative path between two directories (ndk helper style)
relpath() {
	local from=${1%/} to=${2%/} common=$1 up=
	common=$from
	while [ "${to#"$common"/}" = "$to" ] && [ "$common" != "$to" ]; do
		common=${common%/*}
		up="../$up"
		[ -z "$common" ] && break
	done
	local rest=${to#"$common"}
	rest=${rest#/}
	local result="${up}${rest}"
	result=${result%/}
	echo "${result:-.}"
}
relpath /a/b/c /a/b/d/e
relpath /a/b /a/b/c
relpath /a/b/c /a
relpath /a/b /a/b
