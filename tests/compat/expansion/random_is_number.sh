# $RANDOM is a number in 0..32767 (value not printed)
r=$RANDOM
case $r in ''|*[!0-9]*) echo bad ;; *) [ "$r" -le 32767 ] && echo ok ;; esac
