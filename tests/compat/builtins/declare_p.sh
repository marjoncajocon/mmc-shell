# declare -p shows how a variable is defined
s="a \"q\" b"
declare -p s
declare -i n=5; declare -p n
export E=x; declare -p E
declare -p undefined_var 2>/dev/null; echo "status $?"
