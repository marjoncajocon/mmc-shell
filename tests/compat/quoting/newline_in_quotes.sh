# a quoted string may span lines
echo "line one
line two"
x='a
b'
echo "$x" | wc -l
