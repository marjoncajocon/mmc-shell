# mapfile -C runs every -c lines with the index and the line
cb() { echo "cb $1 [$2]"; }
mapfile -t -C cb -c 2 arr < <(printf '%s\n' one two three four five)
echo "${#arr[@]} ${arr[*]}"
