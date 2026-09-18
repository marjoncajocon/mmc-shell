# [[ ]] does not split or glob unquoted variables
v="a b"
[[ $v = "a b" ]] && echo "no split"
e=
[[ -z $e ]] && echo "empty ok"
[[ -n $v && $v != x ]] && echo "&& inside"
[[ -z $v || $v = "a b" ]] && echo "|| inside"
[[ ! -z $v ]] && echo "! inside"
