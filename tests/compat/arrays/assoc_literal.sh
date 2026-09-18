# associative array literal and key existence checks
declare -A m=([one]=1 [two]=2 ["with space"]=3)
[[ -v m[one] ]] && echo "has one"
[[ -v m[three] ]] || echo "no three"
[ -n "${m[two]+x}" ] && echo "two set"
unset 'm[one]'
printf '%s\n' "${!m[@]}" | sort
