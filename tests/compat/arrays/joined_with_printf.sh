# join array elements with a separator using printf and substring
a=(x y z)
s=$(printf ',%s' "${a[@]}")
echo "${s:1}"
