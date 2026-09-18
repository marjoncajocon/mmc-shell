# empty IFS means no word splitting at all
IFS=
x="a b c"
printf '<%s>\n' $x
