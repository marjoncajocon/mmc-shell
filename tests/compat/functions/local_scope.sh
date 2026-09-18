# local variables do not leak; globals are modified
g=global
f() {
	local g=local
	h=set-in-f
	echo "in f: $g"
}
f
echo "after: $g $h"
