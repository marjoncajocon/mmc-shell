# associative arrays with declare -A (keys sorted for output)
declare -A color
color[apple]=red
color[banana]=yellow
color["grape fruit"]=pink
echo "${color[apple]} ${color[grape fruit]}"
echo "${#color[@]}"
for k in "${!color[@]}"; do echo "$k=${color[$k]}"; done | sort
