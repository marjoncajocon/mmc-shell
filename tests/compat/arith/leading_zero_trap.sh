# "08" is invalid octal; 10# forces decimal (common date-parsing fix)
m=08
echo $((10#$m + 1))
( echo $((m + 1)) ) 2>/dev/null
echo "status $?"
