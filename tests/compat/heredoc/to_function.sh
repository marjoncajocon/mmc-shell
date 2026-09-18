# heredoc as stdin of a function
lines() { local n=0; while read -r _; do n=$((n+1)); done; echo "$n lines"; }
lines <<EOF
1
2
3
EOF
