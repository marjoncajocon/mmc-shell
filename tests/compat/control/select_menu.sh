# select reads choices from stdin; menu goes to stderr
select c in red green blue; do
	echo "chose $c ($REPLY)"
	break
done <<< "2"
select c in a b; do
	echo "invalid gives [$c] reply=$REPLY"
	break
done <<< "9"
