# custom IFS with non-whitespace separators keeps empty fields
IFS=:
x="a::b:c:"
printf '<%s>\n' $x
