# unset variables, functions and -v/-f
a=1; b=2
unset a
echo "[${a-unset}] $b"
f() { :; }
unset -f f
type -t f || echo "f gone"
unset -v b
echo "[${b-unset}]"
unset not_set_at_all; echo "status $?"
