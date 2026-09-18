# exported variables reach child processes; plain ones do not
export EXPORTED_V=yes
PLAIN_V=no
env | grep -E '^(EXPORTED_V|PLAIN_V)=' | sort
ONE_SHOT=temp env | grep '^ONE_SHOT='
echo "after: [${ONE_SHOT-unset}]"
