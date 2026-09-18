# for (( )) with break and empty clauses
for ((i = 0; i < 10; i += 3)); do printf '%d ' $i; done; echo
for ((;;)); do echo infinite-once; break; done
