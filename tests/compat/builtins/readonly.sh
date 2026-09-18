# readonly variables cannot be changed or unset
readonly R=1
( R=2 ) 2>/dev/null; echo "assign status $?"
( unset R ) 2>/dev/null; echo "unset status $?"
readonly -p | grep -c ' R='
echo "$R"
