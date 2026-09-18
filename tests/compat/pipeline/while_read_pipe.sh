# producer | while read loop
printf 'x 1\ny 2\n' | while read -r name num; do
	echo "$name:$((num * 10))"
done
