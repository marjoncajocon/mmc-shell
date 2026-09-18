# npm completion.sh style: IFS=$'\n' COMPREPLY=($(...)) with saved IFS
fake_npm() { printf '%s\n' install "run build" test; }
si="$IFS"
if ! IFS=$'\n' COMPREPLY=($(fake_npm)); then
	IFS="$si"
	echo failed
fi
IFS="$si"
echo "${#COMPREPLY[@]}"
printf '[%s]\n' "${COMPREPLY[@]}"
