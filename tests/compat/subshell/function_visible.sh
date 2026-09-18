# functions and variables are inherited by subshells
f() { echo "f in subshell"; }
v=inherited
( f; echo $v )
