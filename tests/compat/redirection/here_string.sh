# <<< here-string, with variables and quoting
x="a b  c"
cat <<< "$x"
cat <<< $x
wc -l <<< "one"
tr a-z A-Z <<< 'lower'
