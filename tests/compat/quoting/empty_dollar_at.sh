# "$@" with no positional parameters produces no words at all
set --
count() { echo $#; }
count "$@"
count "$*"
count "${@}"
count "x$@"
