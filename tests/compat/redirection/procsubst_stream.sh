# <( ) streams: the reader stops an endless writer; >( ) gets all of it
n=0
while read -r l; do n=$((n+1)); [ $n -ge 3 ] && break; done < <(while :; do echo y; done)
echo "read $n lines of an endless one"
mapfile -t arr < <(printf '%s\n' one two three)
echo "mapfile: ${arr[*]}"
source <(echo 'from_source=ok')
echo "source: $from_source"
echo data | tee >(while read -r l; do echo "reader got $l"; done) >/dev/null
echo "to stderr" 2>&1 >/dev/stderr | cat
while read -r a b; do echo "$b $a"; done < <(printf '1 x\n2 y\n')
