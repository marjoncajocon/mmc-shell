# variable changes inside $( ) do not leak out
v=outer
x=$(v=inner; echo $v)
echo "$x $v"
