# set -e only checks the last command of a pipeline (without pipefail)
set -e
false | true
echo "pipeline ok"
set -o pipefail
true | false | true
echo "not reached"
