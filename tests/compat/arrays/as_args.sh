# an array as a command's argument list (safe command building)
args=(-n "hello world")
echo "${args[@]}"; echo
cmd=(printf '%s|')
cmd+=(a "b c")
"${cmd[@]}"; echo
