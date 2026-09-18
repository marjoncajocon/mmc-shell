# only stdout is captured; 2>&1 captures stderr too
x=$(echo out; echo err >&2)
echo "[$x]"
y=$(echo out; echo err >&2) 2>/dev/null
z=$( { echo out; echo err >&2; } 2>&1 )
echo "[$z]"
