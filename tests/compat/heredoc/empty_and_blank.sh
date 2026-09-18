# empty heredoc and heredoc with blank lines
cat <<EOF
EOF
echo ---
cat <<EOF

middle

EOF
echo ---
