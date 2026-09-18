# the right side of an assignment is not split or globbed
x="a   b   *"
y=$x
echo "$y"
z=*
echo "$z"
