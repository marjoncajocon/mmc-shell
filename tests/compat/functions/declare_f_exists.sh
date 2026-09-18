# declare -F checks whether a function exists
cmd_build() { :; }
for c in build test; do
	if declare -F "cmd_$c" >/dev/null; then echo "$c: yes"; else echo "$c: no"; fi
done
declare -F cmd_build
