# EXIT trap runs when set -e aborts the script
set -e
trap 'echo "cleanup (status $?)"' EXIT
echo working
false
echo never
