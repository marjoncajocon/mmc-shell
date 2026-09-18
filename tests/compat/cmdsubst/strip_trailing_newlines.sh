# all trailing newlines are removed, inner ones kept
x=$(printf 'a\nb\n\n\n')
echo "[$x]"
y=$(printf '\n\nlead')
echo "[$y]"
