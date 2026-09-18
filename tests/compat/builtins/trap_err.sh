# ERR trap runs when a command fails
trap 'echo "ERR trap: status $?"' ERR
false
echo continue
(exit 2)
trap - ERR
false
echo end
