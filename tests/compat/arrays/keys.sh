# ${!a[@]} lists indices; loop by index
a=(red green blue)
for i in "${!a[@]}"; do echo "$i=${a[$i]}"; done
