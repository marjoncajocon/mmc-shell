# functions can be redefined and removed with unset -f
f() { echo first; }
f
f() { echo second; }
f
unset -f f
f 2>/dev/null
echo "status $?"
