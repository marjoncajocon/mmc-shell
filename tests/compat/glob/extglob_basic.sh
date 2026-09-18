# extglob: !(p) +(p) @(p) ?(p) *(p)
tmp=$(mktemp -d); cd "$tmp" || exit 1
shopt -s extglob
touch a.c a.h b.c main.o lib.so readme
echo !(*.c)
echo *.@(c|h)
echo +([a-z]).c
echo lib?(.so)
cd / && rm -rf "$tmp"
