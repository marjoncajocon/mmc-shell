# shopt -s lastpipe runs the last part in the current shell (job control off)
shopt -s lastpipe
n=0
printf 'a\nb\n' | while read -r l; do n=$((n + 1)); done
echo "n=$n"
echo hi | read -r v
echo "v=$v"
