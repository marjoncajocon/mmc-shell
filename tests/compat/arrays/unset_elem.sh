# unset removes an element without renumbering
a=(a b c d)
unset 'a[1]'
echo "${#a[@]} ${!a[@]} ${a[*]}"
unset a
echo "${#a[@]}"
