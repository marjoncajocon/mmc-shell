# declare -i -l -u -r -x
declare -l low=MiXeD
declare -u up=MiXeD
echo "$low $up"
low=AGAIN; echo "$low"
declare -r ro=fixed
ro=changed 2>/dev/null
echo "$ro"
declare -x EXP=1
env | grep '^EXP='
