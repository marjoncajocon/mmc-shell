# test returns 0, 1, or 2 on a syntax error
[ 1 -eq 1 ]; echo $?
[ 1 -eq 2 ]; echo $?
[ abc -eq 1 ] 2>/dev/null; echo $?
[ 1 = 1 2>/dev/null; echo $?
