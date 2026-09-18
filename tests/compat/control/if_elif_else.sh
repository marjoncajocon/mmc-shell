# if / elif / else chain
for n in 1 2 3; do
	if [ "$n" -eq 1 ]; then
		echo one
	elif [ "$n" -eq 2 ]; then
		echo two
	else
		echo other
	fi
done
