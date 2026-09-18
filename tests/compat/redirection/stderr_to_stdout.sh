# 2>&1 sends stderr where stdout goes
{ echo out; echo err >&2; } 2>&1 | sort
