# "$@" copied into an array and modified
set -- a b c
args=("$@")
args[1]=B
set -- "${args[@]}"
echo "$*"
