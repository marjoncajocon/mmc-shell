# extract a version number from tool output
out="git version 2.45.2.windows.1"
if [[ $out =~ ([0-9]+)\.([0-9]+)(\.([0-9]+))? ]]; then
	major=${BASH_REMATCH[1]} minor=${BASH_REMATCH[2]} patch=${BASH_REMATCH[4]:-0}
	echo "major=$major minor=$minor patch=$patch"
fi
ver=$(echo "$out" | sed -n 's/^git version \([0-9.]*[0-9]\).*/\1/p')
echo "$ver"
IFS=. read -r a b c _ <<< "$ver"
echo "$((a * 10000 + b * 100 + c))"
