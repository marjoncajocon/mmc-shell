# a function body in ( ) runs in a subshell
x=1
f() ( x=2; cd /; echo "in f x=$x" )
f
echo "after x=$x"
