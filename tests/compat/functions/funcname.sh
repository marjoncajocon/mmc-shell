# FUNCNAME is the call stack
a() { echo "${FUNCNAME[0]} called by ${FUNCNAME[1]}"; b; }
b() { echo "stack: ${FUNCNAME[*]}"; }
a
