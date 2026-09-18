# set -x writes to stderr only; stdout is unchanged
set -x
a=1
echo "value $a"
set +x
echo done
