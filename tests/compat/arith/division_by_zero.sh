# division by zero is an error; status is printed
( echo $((5 / 0)) ) 2>/dev/null
echo "status $?"
