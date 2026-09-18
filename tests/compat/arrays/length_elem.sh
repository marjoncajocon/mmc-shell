# ${#a[n]} is the length of one element
a=(short "much longer")
echo "${#a[0]} ${#a[1]}"
