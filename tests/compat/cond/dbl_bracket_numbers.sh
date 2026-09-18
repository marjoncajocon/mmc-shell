# -eq/-lt in [[ ]] evaluate arithmetic expressions
a=3
[[ $a -eq 3 ]] && echo eq
[[ a+1 -eq 4 ]] && echo "arith operand"
[[ "$a" -gt 1 && "$a" -lt 5 ]] && echo range
