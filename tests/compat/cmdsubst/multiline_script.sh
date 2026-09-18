# multi-line command in $( ) with pipes and comments
r=$(
	# comment inside
	printf '%s\n' c b a |
		sort |
		tr '\n' ' '
)
echo "[$r]"
