# a variable whose value is an expression is evaluated recursively
x='2 + 3'
y=x
echo $((x * 2)) $((y * 2)) $(($x * 2))
