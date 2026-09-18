# backtick substitution, including escaped nested backticks
x=`echo old`
y=`echo \`echo nested\``
echo "$x $y"
z=`echo "a\\b"`
echo "$z"
