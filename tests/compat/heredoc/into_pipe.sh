# heredoc feeding a pipeline
cat <<EOF | sort | tr '\n' ' '
c
a
b
EOF
echo
