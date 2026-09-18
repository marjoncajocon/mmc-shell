# git filter-branch style: while read commit parents; count with $(( ))
count=0
while read commit parents; do
	count=$(($count + 1))
	set -- $parents
	echo "$commit has $# parent(s)"
done <<EOF
c3 c2
c2 c1 c0
c1
EOF
echo "total $count"
