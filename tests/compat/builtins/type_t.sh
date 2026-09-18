# type -t classifies names
f() { :; }
alias ll='ls -l'
for n in if echo f cat no_such_thing_xyz; do
	printf '%s: %s\n' "$n" "$(type -t "$n" || echo none)"
done
