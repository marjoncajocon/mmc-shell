# in an unquoted heredoc \$ and \` and \\ are escapes, other backslashes stay
x=val
cat <<EOF
\$x is $x
back\\slash \t stays
quote " and ' stay
EOF
