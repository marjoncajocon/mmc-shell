# ( ) grouping and precedence of && over ||
a=1 b=2
[[ ( $a = 1 || $b = 9 ) && $b = 2 ]] && echo grouped
[[ $a = 9 || $a = 1 && $b = 9 ]] || echo "&& binds tighter"
