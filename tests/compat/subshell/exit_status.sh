# exit in a subshell only ends the subshell
( exit 4 ); echo "status $?"
( false ) || echo "failed subshell"
(echo a; exit 0; echo b)
