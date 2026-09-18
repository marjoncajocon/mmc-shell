# build an array in a loop then join
out=()
for f in a.c b.h c.c; do
	case $f in *.c) out+=("${f%.c}.o") ;; esac
done
echo "${out[*]}"
echo "${#out[@]}"
