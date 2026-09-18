# negative indices count from the end
a=(1 2 3 4)
echo "${a[-1]} ${a[-2]}"
echo "${a[@]: -2}"
