# command runs the builtin/external even when a function has the same name
echo() { builtin echo "wrapped: $*"; }
echo hi
command echo plain
builtin echo builtin
unset -f echo
cd() { builtin cd "$@" && echo "cd done"; }
cd /
