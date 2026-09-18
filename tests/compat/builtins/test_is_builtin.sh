# test and [ are builtins; ] is required
type -t test [ [[
[ 1 = 1 2>/dev/null
echo "missing ] status $?"
