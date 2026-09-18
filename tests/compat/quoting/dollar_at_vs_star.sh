# "$@" keeps args, "$*" joins with first char of IFS, $@/$* split again
set -- "a b" c "d  e"
printf '1<%s>\n' "$@"
printf '2<%s>\n' "$*"
printf '3<%s>\n' $@
IFS=,
printf '4<%s>\n' "$*"
