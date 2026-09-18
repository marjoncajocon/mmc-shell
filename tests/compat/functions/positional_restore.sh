# set -- inside a function does not change the caller's args
f() { set -- x y z; echo "in: $*"; }
set -- a b
f
echo "out: $*"
