# braces expand before variables
a=x b=y
echo {$a,$b}
echo "${a}"{1,2}
