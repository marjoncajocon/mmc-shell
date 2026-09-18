# BASH_SUBSHELL counts nesting
echo $BASH_SUBSHELL
( echo $BASH_SUBSHELL; ( echo $BASH_SUBSHELL ) )
echo $(echo $BASH_SUBSHELL)
