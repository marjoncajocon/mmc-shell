# type output for builtins, keywords and functions
type echo
type if
greet() { echo hi; }
type greet
type no_such_thing_xyz >/dev/null 2>&1; echo "status $?"
