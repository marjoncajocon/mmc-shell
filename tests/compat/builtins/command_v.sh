# command -v for builtins/functions/missing; status only for files
f() { :; }
command -v echo
command -v f
command -v cat >/dev/null && echo "cat found"
command -v no_such_cmd_xyz; echo "status $?"
