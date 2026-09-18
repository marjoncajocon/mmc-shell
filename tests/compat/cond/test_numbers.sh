# numeric comparisons -eq -ne -lt -le -gt -ge
for n in 1 5 10; do
	r=
	[ $n -eq 5 ] && r="$r eq"
	[ $n -ne 5 ] && r="$r ne"
	[ $n -lt 5 ] && r="$r lt"
	[ $n -le 5 ] && r="$r le"
	[ $n -gt 5 ] && r="$r gt"
	[ $n -ge 5 ] && r="$r ge"
	echo "$n:$r"
done
