# set -- replaces positional parameters
set -- a "b c" d
echo "$# [$2]"
set --
echo "$#"
set -- "$@" x
set -- "$@" y
echo "$*"
