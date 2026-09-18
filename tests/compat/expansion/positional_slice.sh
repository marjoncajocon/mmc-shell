# ${@:n} and ${@:n:len} slice the positional parameters
set -- a b c d e
echo "${@:2}"
echo "${@:2:2}"
echo "${@: -2}"
echo "${@:0:1}" | sed 's/.*\.sh$/SCRIPT/'
printf '<%s>\n' "${@:4}"
