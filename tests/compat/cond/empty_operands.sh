# empty and blank operands in [[ ]]
[[ "" == "" ]] && echo "empty equal"
[[ -z "" && -n " " ]] && echo "empty and blank operands"
[[ "" < a ]] && echo "empty sorts first"
