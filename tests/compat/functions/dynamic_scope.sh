# a callee sees the caller's local variables (dynamic scoping)
inner() { echo "inner sees x=$x"; x=changed; }
outer() { local x=outer-local; inner; echo "outer now x=$x"; }
x=global
outer
echo "global x=$x"
