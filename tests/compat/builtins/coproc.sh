# coproc: a background command with a pipe each way
coproc { while read -r line; do echo "got: $line"; done; }
echo "hello" >&"${COPROC[1]}"
read -r reply <&"${COPROC[0]}"
echo "$reply"
echo "world" >&"${COPROC[1]}"
read -r reply <&"${COPROC[0]}"
echo "$reply"
[ -n "$COPROC_PID" ] && echo "pid ok"
exec {COPROC[1]}>&-
wait "$COPROC_PID"
echo "coproc ended: $?"

coproc UPPER { while read -r l; do echo "${l^^}"; done; }
echo "shout" >&"${UPPER[1]}"
exec {UPPER[1]}>&-
read -r big <&"${UPPER[0]}"
echo "$big"
wait "$UPPER_PID"
echo "named: $?"

coproc cat
echo "via cat" >&"${COPROC[1]}"
read -r line <&"${COPROC[0]}"
echo "$line"
exec {COPROC[1]}>&-
wait
echo done
