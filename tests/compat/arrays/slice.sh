# ${a[@]:off:len} slices
a=(a b c d e)
echo "${a[@]:1:3}"
echo "${a[@]:3}"
printf '<%s>' "${a[@]:0:2}"; echo
