# a && b || c is left-associative
false && echo A || echo B
true && false || echo C
true || false && echo D
