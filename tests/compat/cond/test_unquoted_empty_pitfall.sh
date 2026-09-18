# classic pitfalls: [ -n $empty ] is true, [ $empty = x ] is an error
e=
[ -n $e ] && echo "-n with unquoted empty is TRUE"
[ -n "$e" ] || echo "quoted is false"
[ $e = x ] 2>/dev/null; echo "status $?"
