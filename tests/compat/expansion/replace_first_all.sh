# ${v/pat/rep} first match, ${v//pat/rep} all matches
s="one two one two"
echo "${s/one/1}"
echo "${s//one/1}"
echo "${s//o/}"
echo "${s/nomatch/x}"
