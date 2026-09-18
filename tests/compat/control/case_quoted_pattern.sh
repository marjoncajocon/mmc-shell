# quoted pattern parts match literally, unquoted variables are patterns
pat='*.txt'
for s in a.txt '*.txt'; do
	case $s in "$pat") echo "$s literal" ;; *) echo "$s no-literal" ;; esac
	case $s in $pat) echo "$s glob" ;; esac
done
