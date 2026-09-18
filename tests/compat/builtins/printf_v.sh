# printf -v assigns to a variable (git-prompt style)
printf -v out '%s-%03d' build 7
echo "$out"
printf -v pad '%*s' 5 ''
echo "[${pad// /.}]"
declare -a arr
printf -v 'arr[1]' '%s' elem
echo "${arr[1]}"
