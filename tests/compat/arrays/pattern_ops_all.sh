# case and replace operations on all elements
a=(alpha beta gamma)
echo "${a[@]^}"
echo "${a[@]/a/A}"
echo "${a[@]:0:2}"
