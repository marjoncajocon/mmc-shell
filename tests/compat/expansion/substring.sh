# ${v:off} and ${v:off:len}
s=abcdefgh
echo "${s:2}"
echo "${s:2:3}"
echo "${s:0:1}"
echo "[${s:20}]"
n=3
echo "${s:n:n}"
echo "${s:$((n-1)):2}"
