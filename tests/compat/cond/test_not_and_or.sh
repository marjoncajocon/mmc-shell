# ! -a -o and \( \) in test
[ ! -z x ] && echo not
[ a = a -a b = b ] && echo and
[ a = x -o b = b ] && echo or
[ \( a = a -o a = b \) -a c = c ] && echo parens
