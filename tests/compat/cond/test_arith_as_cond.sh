# (( )) as a condition in if and &&
n=4
if (( n % 2 == 0 )); then echo even; fi
(( n > 10 )) || echo small
