# mapfile -n, -s options
mapfile -t -s 1 -n 2 arr < <(printf '%s\n' a b c d)
echo "${arr[*]}"
