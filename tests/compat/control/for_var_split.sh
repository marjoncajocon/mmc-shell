# for over an unquoted variable splits on IFS
list="a b c"
for w in $list; do printf '%s.' "$w"; done; echo
IFS=,
csv="1,2,3"
for w in $csv; do printf '%s;' "$w"; done; echo
