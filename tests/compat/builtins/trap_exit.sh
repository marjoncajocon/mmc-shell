# EXIT trap runs at the end of the script, even after exit N
trap 'echo "exit trap, status $?"' EXIT
echo body
exit 3
