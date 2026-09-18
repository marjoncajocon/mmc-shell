# positional parameters inside a function; $0 stays the script
show() { echo "n=$# first=$1 all=$*"; basename "$0"; }
show a "b c" d
set -- outer
show
echo "after: $1"
