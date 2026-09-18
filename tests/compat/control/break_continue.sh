# break and continue in a loop
for i in 1 2 3 4 5 6; do
	[ $i -eq 2 ] && continue
	[ $i -eq 5 ] && break
	echo $i
done
