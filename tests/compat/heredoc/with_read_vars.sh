# read several variables from a heredoc
read -r a b c <<EOF
1 2 3 4
EOF
echo "$a|$b|$c"
