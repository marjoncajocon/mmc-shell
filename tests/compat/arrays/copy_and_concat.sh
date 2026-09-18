# copy arrays and concatenate them
a=(1 "2 3")
b=("${a[@]}")
c=("${a[@]}" "${b[@]}" 4)
echo "${#b[@]} ${#c[@]}"
printf '<%s>' "${c[@]}"; echo
