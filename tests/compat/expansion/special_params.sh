# $# $? $0 basename and $1..$9 and ${10}
set -- 1 2 3 4 5 6 7 8 9 ten eleven
echo "$#"
echo "$9 ${10} ${11} $10"
false; echo "$?"
basename "$0"
