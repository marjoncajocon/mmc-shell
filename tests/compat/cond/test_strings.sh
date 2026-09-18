# test / [ ] string comparisons: = != -z -n
a=abc e=
[ "$a" = abc ] && echo eq
[ "$a" != xyz ] && echo ne
[ -z "$e" ] && echo "e empty"
[ -n "$a" ] && echo "a nonempty"
test "$a" = abc && echo "test builtin"
[ "$a" == abc ] && echo "== works in bash ["
