# set +e turns errexit off again
set -e
set +e
false
echo "continued after false"
set -eu
set +eu
echo "${nothing_here}ok"
