# read returns non-zero at EOF but still sets a partial last line
printf 'first\nlast-no-newline' | {
	while read -r l; do echo "loop: $l"; done
	echo "leftover: [$l]"
}
printf 'a\nb' | while IFS= read -r l || [ -n "$l" ]; do echo "got $l"; done
read -r x < /dev/null; echo "status $?"
