# &> and &>> redirect both streams
tmp=$(mktemp -d)
{ echo o1; echo e1 >&2; } &> "$tmp/f"
{ echo o2; echo e2 >&2; } &>> "$tmp/f"
sort "$tmp/f"
rm -rf "$tmp"
