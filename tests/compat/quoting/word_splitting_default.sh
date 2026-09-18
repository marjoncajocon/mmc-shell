# unquoted expansion is split on space/tab/newline and runs are collapsed
x="  one   two
three	four  "
printf '<%s>\n' $x
printf '<%s>\n' "$x" | head -1
