# traps are reset in subshells, but EXIT trap of the subshell runs
trap 'echo "outer exit"' EXIT
( trap 'echo "inner exit"' EXIT; echo "in subshell" )
echo main
