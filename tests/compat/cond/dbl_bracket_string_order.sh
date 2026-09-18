# < and > compare strings in [[ ]]
[[ abc < abd ]] && echo lt
[[ b > a ]] && echo gt
[[ 10 < 9 ]] && echo "string compare, not numeric"
