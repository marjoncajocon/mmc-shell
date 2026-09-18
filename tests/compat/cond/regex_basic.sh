# =~ with BASH_REMATCH groups
v="version 12.34.5"
if [[ $v =~ ([0-9]+)\.([0-9]+)\.([0-9]+) ]]; then
	echo "all=${BASH_REMATCH[0]} major=${BASH_REMATCH[1]} minor=${BASH_REMATCH[2]} patch=${BASH_REMATCH[3]}"
fi
[[ abc =~ ^[0-9]+$ ]] || echo "not a number"
