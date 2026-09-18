# ${v:=word} assigns when unset or empty; ${v=word} only when unset
unset a; b=
echo "${a:=one} ${b:=two}"
echo "$a $b"
unset c; d=
echo "${c=three} [${d=four}]"
echo "$c [$d]"
