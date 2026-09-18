# until with a command condition
n=0
until [ "$n" = 3 ] || false; do n=$((n+1)); done
echo $n
