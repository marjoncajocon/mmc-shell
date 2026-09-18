# a quoted pattern part is matched literally
s='a*b*c'
p='*'
echo "${s#*"$p"}"
echo "${s#$p}"
echo "${s%"*c"}"
