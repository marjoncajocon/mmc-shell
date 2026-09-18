# eval to create and read variables with computed names
for n in one two; do eval "var_$n=\"value $n\""; done
echo "$var_one / $var_two"
name=var_two
eval "echo \"\$$name\""
