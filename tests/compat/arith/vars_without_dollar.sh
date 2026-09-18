# variable names inside $(( )) need no $; unset/empty count as 0
a=5 b=3
unset u
e=
echo $((a * b)) $(($a * $b)) $((u + 1)) $((e + 2))
