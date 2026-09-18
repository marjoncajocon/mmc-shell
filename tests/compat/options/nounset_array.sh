# set -u with empty arrays and ${arr[@]+...}
set -u
a=()
echo "count ${#a[@]}"
echo "safe: [${a[@]+"${a[@]}"}]"
( echo "${a[@]}" ) 2>/dev/null; echo "empty array expansion status $?"
