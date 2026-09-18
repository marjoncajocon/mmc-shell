# nested while-read and for loops
while read -r row; do
	for c in $row; do printf '%s' "$c"; done
	echo
done <<EOF
1 2 3
4 5
EOF
