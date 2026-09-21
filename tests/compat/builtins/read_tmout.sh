# $TMOUT is the timeout of read when -t does not give one
TMOUT=1
if read -r x < <(sleep 3; echo late); then echo "read: $x"; else echo "timed out: $(( $? > 128 ))"; fi
