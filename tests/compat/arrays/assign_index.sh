# assigning by index creates sparse arrays
a[5]=five
a[1]=one
echo "${#a[@]}"
echo "${!a[@]}"
echo "${a[@]}"
b=([2]=x [0]=y z)
echo "${!b[@]} -> ${b[*]}"
