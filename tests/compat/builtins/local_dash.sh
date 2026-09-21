# local -: set options come back when the function returns
f() { local -; set -f; set -u; echo "in: $-"; }
echo "before: $-"
f
echo "after: $-"
g() { local - x=1; set -o noclobber; [[ -o noclobber ]] && echo "noclobber on in g, x=$x"; }
g
[[ -o noclobber ]] || echo "noclobber off again"
h() { local x; echo "no inherit: [${x-unset}]"; }
k() { shopt -s localvar_inherit; local x; echo "inherit: [$x]"; shopt -u localvar_inherit; }
x=outer
h
k
