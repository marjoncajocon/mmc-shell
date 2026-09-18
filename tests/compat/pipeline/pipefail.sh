# set -o pipefail: status is the last non-zero status
set -o pipefail
false | true; echo $?
(exit 2) | (exit 3) | true; echo $?
true | true; echo $?
