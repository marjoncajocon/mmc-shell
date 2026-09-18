# ((a[k]++)) on associative and indexed arrays
declare -A h
((h[x]++)); ((h[x]++)); ((h[y] += 5))
echo "${h[x]} ${h[y]}"
a=()
((a[3] = 7))
echo "${a[3]} ${#a[@]}"
