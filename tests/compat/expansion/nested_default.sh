# defaults can nest and contain expansions
unset a b
c=third
echo "${a:-${b:-${c:-none}}}"
echo "${a:-$c/sub}"
echo "${a:-"quoted  spaces"}"
