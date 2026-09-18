# functions as predicates in if and &&
is_even() { [ $(($1 % 2)) -eq 0 ]; }
for n in 1 2 3 4; do
	if is_even $n; then echo "$n even"; else echo "$n odd"; fi
done
is_even 6 && echo "6 even"
