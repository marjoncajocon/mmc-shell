# $- holds option letters; $_ holds last arg of previous command
case $- in *h*) echo has-h ;; *) echo no-h ;; esac
set -e
case $- in *e*) echo has-e ;; esac
set +e
echo one two three >/dev/null
echo "$_"
