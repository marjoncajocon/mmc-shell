# negative offsets need a space or parentheses; negative length counts from end
s=abcdefgh
echo "${s: -3}"
echo "${s:(-3):2}"
echo "${s:1:-2}"
echo "${s:-3}"
