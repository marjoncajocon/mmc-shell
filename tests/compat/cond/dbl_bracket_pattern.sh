# == with an unquoted right side is a glob pattern
f=main.c
[[ $f == *.c ]] && echo "c file"
[[ $f == m??n.? ]] && echo "question marks"
[[ $f == [mn]* ]] && echo "bracket"
[[ $f != *.h ]] && echo "not h"
[[ $f == "*.c" ]] || echo "quoted is literal"
