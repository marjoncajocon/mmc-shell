# read -d sets the delimiter; -d '' reads to NUL/EOF
read -r -d , first <<< "x,y,z"
echo "$first"
read -r -d '' all <<EOF
line1
line2
EOF
echo "status $?"
echo "$all"
printf 'a\0b\0' | while IFS= read -r -d '' item; do echo "item $item"; done
