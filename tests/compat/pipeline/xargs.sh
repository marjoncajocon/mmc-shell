# xargs builds command lines from input
printf '%s\n' a b c | xargs echo
printf '%s\n' 1 2 3 4 | xargs -n 2 echo pair
