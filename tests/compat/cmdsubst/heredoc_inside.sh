# here-document inside $( ) (git-mergetool--lib style)
functions=$(cat << \EOF
one() { :; }
two() { :; }
EOF
)
echo "$functions"
