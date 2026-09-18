# ${v:?msg} fails (non-interactive shell exits) when unset or empty
s=ok
echo "${s:?no}"
( unset u; echo "${u:?u is required}"; echo notreached )
echo "status $?"
( e=; : "${e?only unset fails}"; echo empty-ok )
