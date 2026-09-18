# set -e ignores failures in conditions, && lists and with !
set -e
if false; then :; fi
false || true
false && true
! true
while false; do :; done
echo "still running"
x=$(false) || echo "cmdsubst failure handled"
echo end
