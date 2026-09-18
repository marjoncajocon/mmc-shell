# backslash-newline in an unquoted heredoc joins lines
cat <<EOF
one \
two
EOF
cat <<'EOF'
kept \
as is
EOF
