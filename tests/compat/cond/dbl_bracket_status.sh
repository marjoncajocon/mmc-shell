# status of [[ ]]; used as a command
[[ a = a ]]; echo $?
[[ a = b ]]; echo $?
x=$([[ 1 -lt 2 ]] && echo yes || echo no)
echo $x
