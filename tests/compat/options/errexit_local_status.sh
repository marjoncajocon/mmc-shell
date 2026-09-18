# set -e with local x=$(false) does not exit (local succeeds)
set -e
f() { local v=$(false); echo "survived local"; }
f
echo end
