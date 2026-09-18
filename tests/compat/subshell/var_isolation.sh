# variables set in ( ) do not leak
x=1
( x=2; echo "inner $x" )
echo "outer $x"
