# local x=$(cmd) returns local's status, not cmd's
f() {
	local a=$(false); echo "local: $?"
	local b
	b=$(false); echo "separate: $?"
}
f
