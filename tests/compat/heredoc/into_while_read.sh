# heredoc as input to a while read loop
while IFS=: read -r user shell; do
	echo "$user uses $shell"
done <<EOF
alice:bash
bob:zsh
EOF
