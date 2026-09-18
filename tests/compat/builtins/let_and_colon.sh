# : is a no-op returning 0; true/false
: this is ignored; echo $?
true; echo $?
false; echo $?
: > /dev/null && echo colon-redirect
