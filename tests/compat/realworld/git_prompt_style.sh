# git-prompt style: printf -v with conditional pieces and local vars
ps1_git() {
	local b=$1 dirty=$2 upstream=$3
	local w= u= gitstring
	[ "$dirty" = 1 ] && w="*"
	case "$upstream" in
		"0 0") u="=" ;;
		"0 "*) u=">" ;;
		*" 0") u="<" ;;
		*) u="<>" ;;
	esac
	printf -v gitstring " (%s%s%s)" "$b" "${w:+ $w}" "${u:+ $u}"
	echo "$gitstring"
}
ps1_git main 0 "0 0"
ps1_git feature 1 "0 3"
ps1_git fix 1 "2 0"
ps1_git dev 0 "1 1"
