# an invalid number prints 0 and returns status 1
printf '%d\n' abc 2>/dev/null
echo "status $?"
printf '%d\n' 12abc 2>/dev/null
echo "status $?"
