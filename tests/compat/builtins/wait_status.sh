# wait returns the exit status of a background job
( exit 7 ) &
pid=$!
wait "$pid"; echo "status $?"
