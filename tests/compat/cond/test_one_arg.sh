# one-argument test is true for a non-empty string
[ x ] && echo "x true"
[ "" ] || echo "empty false"
[ -n ] && echo "-n alone is a string"
[ ] || echo "no args false"
