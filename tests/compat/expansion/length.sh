# ${#var} is the length in characters
x=hello
e=
echo "${#x} ${#e} ${#undefined_var}"
set -- a bb ccc
echo "${#1} ${#@} ${#*}"
