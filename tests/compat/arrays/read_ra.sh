# read -ra splits a line into an array
IFS=: read -ra parts <<< "/usr/bin:/bin:/usr/local/bin"
echo "${#parts[@]}"
printf '%s\n' "${parts[@]}"
read -ra w <<< "  lots   of   space  "
echo "${#w[@]} ${w[2]}"
