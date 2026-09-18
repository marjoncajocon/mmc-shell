# sort array elements through a pipe
a=(pear apple "fig tree" banana)
IFS=$'\n' sorted=($(printf '%s\n' "${a[@]}" | sort))
unset IFS
printf '%s\n' "${sorted[@]}"
