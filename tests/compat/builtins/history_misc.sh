# history -p does history expansion; $HISTCMD; $- with -c comes from the runner
echo "hc=$HISTCMD"
x=$(TMOUT=1 bash_or_mmc=1; echo ok)
echo "$x"
set -o | grep -c . >/dev/null && echo "set -o lists"
