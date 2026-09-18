# each part of a pipeline runs in a subshell: vars do not leak
n=0
printf 'a\nb\n' | while read -r l; do n=$((n + 1)); done
echo "n=$n"
