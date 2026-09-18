# trap - resets; trap -p prints current traps
trap 'echo bye' EXIT
trap -p EXIT
trap - EXIT
trap -p EXIT
echo "no trap left"
