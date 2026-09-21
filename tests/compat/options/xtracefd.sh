# BASH_XTRACEFD sends the set -x trace to another fd
exec 5>&1
BASH_XTRACEFD=5
set -x
echo traced
set +x
echo done
