# read -n N reads N characters; -N ignores delimiters
read -n 3 part <<< "abcdef"
echo "$part"
read -N 4 raw <<< $'ab\ncd'
echo "[$raw]"
