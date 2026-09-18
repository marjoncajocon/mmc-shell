# aliases are not expanded in scripts unless expand_aliases is set
alias hi='echo alias-hi'
hi 2>/dev/null || echo "not expanded"
shopt -s expand_aliases
alias hi2='echo alias-hi2'
hi2
unalias hi2
alias hi
