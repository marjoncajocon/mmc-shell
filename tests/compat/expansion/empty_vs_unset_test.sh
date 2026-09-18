# ${v+x} distinguishes unset from empty
unset a; b=
[ -z "${a+x}" ] && echo "a unset"
[ -n "${b+x}" ] && echo "b set but empty"
