# IFS with whitespace and non-whitespace: " , " counts as one separator
IFS=' ,'
x="a , b,,c ,d"
printf '<%s>\n' $x
