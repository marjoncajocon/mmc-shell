# local outside a function is an error
local x=1 2>/dev/null
echo "status $?"
