# prefix/suffix/replace operators apply per element of "${arr[@]}"
files=(a.c b.c dir/c.c)
echo "${files[@]%.c}"
echo "${files[@]/#/src/}"
echo "${files[@]##*/}"
