# while IFS= read -r line; do ... done < file (reads this script)
n=0 comments=0
while IFS= read -r line; do
	n=$((n + 1))
	case $line in \#*) comments=$((comments + 1)) ;; esac
done < "$0"
echo "lines=$n comments=$comments"
