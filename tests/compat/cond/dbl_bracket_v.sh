# -v tests whether a variable is set
x=1; e=
unset u
[[ -v x ]] && echo "x set"
[[ -v e ]] && echo "e set (empty)"
[[ -v u ]] || echo "u unset"
a=(1 2)
[[ -v a[1] ]] && echo "a[1] set"
[[ -v a[5] ]] || echo "a[5] unset"
