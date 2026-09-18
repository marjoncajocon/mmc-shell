# process files found with a glob, skipping missing matches and spaces
tmp=$(mktemp -d)
mkdir -p "$tmp/src"
for n in "a.c" "b c.c" "d.h"; do echo "// $n" > "$tmp/src/$n"; done
count=0
for f in "$tmp"/src/*.c; do
	[ -e "$f" ] || continue
	count=$((count + 1))
	printf '%s: %s\n' "$(basename "$f")" "$(head -1 "$f")"
done
echo "$count C files"
for f in "$tmp"/src/*.none; do [ -e "$f" ] || { echo "skipped non-match"; continue; }; done
rm -rf "$tmp"
