# read -a into an array
read -r -a arr <<< "x y  z"
echo "${#arr[@]} ${arr[2]}"
