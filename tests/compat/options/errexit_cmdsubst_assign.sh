# set -e: a failing $( ) in a plain assignment exits
set -e
echo start
x=$(exit 3)
echo "not reached"
