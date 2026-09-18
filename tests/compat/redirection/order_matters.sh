# >f 2>&1 vs 2>&1 >f
tmp=$(mktemp -d)
{ echo O; echo E >&2; } > "$tmp/a" 2>&1
echo "a: $(sort "$tmp/a" | tr '\n' ' ')"
{ { echo O; echo E >&2; } 2>&1 > "$tmp/b"; } 2>/dev/null | sed 's/^/pipe: /'
echo "b: $(cat "$tmp/b")"
rm -rf "$tmp"
