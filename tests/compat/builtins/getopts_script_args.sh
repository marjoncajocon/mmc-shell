# getopts over the script's own arguments after set --
set -- -n 3 -q name
n=1 quiet=no
while getopts "n:q" o; do
	case $o in n) n=$OPTARG ;; q) quiet=yes ;; esac
done
shift $((OPTIND - 1))
echo "n=$n quiet=$quiet name=$1 OPTIND=$OPTIND"
