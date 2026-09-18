# eval "set -- $quoted" to restore a quoted argument list
quoted="'a b' 'c' 'it'\\''s'"
eval "set -- $quoted"
echo "$#"
printf '<%s>\n' "$@"
