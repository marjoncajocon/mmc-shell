# eval returns the status of the last command; empty eval is 0
eval false; echo $?
eval ''; echo $?
eval 'exit 5' ; echo "not reached"
