# set -e: a failing subshell exits the parent
set -e
( echo inner; false )
echo "not reached"
