# set -euo pipefail strict mode script
set -euo pipefail
IFS=$'\n\t'
items=$(printf '%s\n' b a c | sort)
for i in $items; do echo "item $i"; done
count=$(printf 'x\ny\n' | wc -l)
echo "count $((count))"
grep -q zzz <<< "abc" || echo "grep miss handled"
missing=${MAYBE_UNSET:-fallback}
echo "$missing"
false | true
echo "not reached"
