# double quotes nest inside $( ) inside double quotes
x="a  b"
echo "out: $(echo "in: $x")"
echo "$(echo "$(echo "deep  spaces")")"
