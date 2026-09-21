# wait -p NAME gets the pid; jobs -r and -s filter
true &
p=$!
wait -p who "$p"
[ "$who" = "$p" ] && echo "wait -p: the pid"
true &
wait -n -p who2
[ -n "$who2" ] && echo "wait -n -p: a pid"
jobs -s; echo "jobs -s: $?"
