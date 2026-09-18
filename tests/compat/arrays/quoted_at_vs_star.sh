# "${a[@]}" keeps elements, "${a[*]}" joins with IFS[0]
a=("x y" z)
printf '<%s>\n' "${a[@]}"
printf '<%s>\n' "${a[*]}"
printf '<%s>\n' ${a[@]}
IFS=-
echo "${a[*]}"
