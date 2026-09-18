# variables used inside ${ } offsets are evaluated arithmetically
s=0123456789
a=2 b=3
echo "${s:a+b:a}"
