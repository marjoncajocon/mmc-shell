# heredoc on a different fd
cat 3<<EOF <&3
via fd 3
EOF
