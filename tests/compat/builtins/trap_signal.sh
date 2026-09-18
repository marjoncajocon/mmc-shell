# a trap on a signal sent to the shell itself
trap 'echo "got USR1"' USR1
kill -USR1 $$
echo after
trap 'echo "got TERM"; exit 0' TERM
kill -TERM $$
echo "not reached"
