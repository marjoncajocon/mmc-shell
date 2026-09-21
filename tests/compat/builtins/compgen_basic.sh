# compgen: the candidates a completion would give
compgen -W "alpha beta gamma alps" a
echo "status $?"
compgen -W "alpha beta" z
echo "status $?"
compgen -W "aa ab" -P "<" -S ">" a
compgen -W "a.c b.o c.c" -X '*.o' ''
compgen -W "one two three" -- t
_gen() { COMPREPLY=(red green blue); }
compgen -F _gen -- x 2>/dev/null
IFS=,
compgen -W "p,q,r" ''
unset IFS
