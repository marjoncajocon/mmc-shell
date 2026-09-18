# set -e exits on the first failing simple command
set -e
echo one
false
echo "not reached"
