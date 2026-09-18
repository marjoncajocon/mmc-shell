# mapfile without -t keeps the newlines
mapfile arr <<< $'a\nb'
printf '%s' "${arr[@]}"
echo "${#arr[0]}"
