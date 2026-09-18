# $( ) nests freely
echo $(echo $(echo $(echo deep)))
d=$(basename "$(dirname "/a/b/c.txt")")
echo "$d"
