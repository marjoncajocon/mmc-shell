# command -v x >/dev/null 2>&1 || ... dependency checks
need() {
	command -v "$1" >/dev/null 2>&1 || { echo "missing: $1"; return 1; }
	echo "have: $1"
}
need sort
need no_such_tool_abc
missing=0
for t in cat grep no_such_1 no_such_2; do
	command -v "$t" >/dev/null 2>&1 || missing=$((missing + 1))
done
echo "missing count: $missing"
if ! hash no_such_tool_abc 2>/dev/null; then echo "hash says missing"; fi
