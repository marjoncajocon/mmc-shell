# a function defined inside another exists after the outer runs
outer() { inner() { echo "inner defined"; }; }
type -t inner || echo "not yet"
outer
inner
