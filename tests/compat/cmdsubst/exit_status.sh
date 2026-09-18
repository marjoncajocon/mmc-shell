# status of assignment is the status of the substitution
x=$(false); echo $?
x=$(true); echo $?
x=$(exit 7); echo $?
echo "$(exit 3)" >/dev/null; echo $?
