# { } runs in the current shell, ( ) in a subshell
x=0
{ x=1; }
echo $x
( x=2 )
echo $x
