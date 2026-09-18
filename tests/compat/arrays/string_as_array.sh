# a scalar behaves as element 0 of an array
s=hello
echo "${s[0]} ${#s[@]} [${s[1]}]"
s[1]=world
echo "${s[@]}"
