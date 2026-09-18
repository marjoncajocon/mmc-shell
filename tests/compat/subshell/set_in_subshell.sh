# set -e / functions / traps inside a subshell stay there
( set -e; false; echo "not printed" )
echo "subshell status $?"
case $- in *e*) echo "e leaked" ;; *) echo "no e outside" ;; esac
