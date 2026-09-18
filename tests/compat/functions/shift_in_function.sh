# shift inside a function works on the function's args
f() { local first=$1; shift; echo "first=$first rest=$*"; }
f 1 2 3
