# ${v#pat} shortest, ${v##pat} longest prefix removal
p=/usr/local/lib/libfoo.so.1
echo "${p#*/}"
echo "${p##*/}"
echo "${p#/usr}"
echo "${p#nomatch}"
