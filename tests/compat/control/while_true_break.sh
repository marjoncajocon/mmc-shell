# while true / while : with break
n=0
while true; do n=$((n+1)); [ $n -ge 3 ] && break; done
echo $n
while :; do echo once; break; done
