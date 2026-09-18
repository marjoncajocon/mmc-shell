# unquoted delimiter: variables, $( ) and $(( )) expand
name=World
cat <<EOF
Hello, $name!
Sum: $((2 + 3))
Cmd: $(echo sub)
EOF
