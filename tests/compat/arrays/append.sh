# += appends elements or strings
a=(1 2)
a+=(3 "4 5")
echo "${#a[@]}: ${a[*]}"
a[0]+=x
echo "${a[0]}"
b=()
b+=(first)
echo "${b[@]}"
