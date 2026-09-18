# [ ] && action idiom and set -- default
set --
[ $# -eq 0 ] && set -- "-"
echo "$@"
[ "$*" != "-" ] && echo notdash || echo dash
