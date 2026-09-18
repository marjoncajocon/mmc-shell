# OPTIND must be reset to parse a second argument list
set -- -a
getopts a o; echo "$o $OPTIND"
set -- -b
OPTIND=1
getopts b o; echo "$o $OPTIND"
