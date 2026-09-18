# return outside a function/sourced file is an error (script continues)
return 2>/dev/null
echo "status $?"
