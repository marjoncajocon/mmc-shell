# ${!name} indirect expansion
name=target
target="the value"
echo "${!name}"
i=2
set -- x y z
echo "${!i}"
