# |& pipes stderr and stdout
{ echo out; echo err >&2; } |& sort
