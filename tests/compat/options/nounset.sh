# set -u: unset variables are errors, defaults are fine
set -u
x=1
echo "$x ${unset_var:-default} ${unset_var-} [${#@}]"
( echo "$really_unset" ) 2>/dev/null
echo "status $?"
set --
echo "empty \$@ ok: [$*] [$@]"
