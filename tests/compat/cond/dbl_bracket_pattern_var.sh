# pattern stored in a variable: unquoted is a pattern, quoted is literal
p='*.txt'
f=notes.txt
[[ $f == $p ]] && echo "pattern match"
[[ $f == "$p" ]] || echo "no literal match"
