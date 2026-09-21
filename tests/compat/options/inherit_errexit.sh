# set -e is off inside $( ) unless shopt -s inherit_errexit
set -e
x=$(false; echo still)
echo "x=$x"
y=$(shopt -s inherit_errexit; set -e; false; echo not here) || true
echo "y=[$y]"
shopt -s inherit_errexit
z=$(false; echo nope) || echo "the substitution failed"
echo "z=[$z]"
