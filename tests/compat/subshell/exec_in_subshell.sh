# exec replaces the subshell process
( exec echo "exec'd"; echo "not here" )
echo after
