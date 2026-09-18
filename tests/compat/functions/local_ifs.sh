# local IFS limits an IFS change to the function
split() { local IFS=:; set -- $1; echo "$# parts"; }
split "a:b:c"
x="a:b c"
set -- $x
echo "$# words outside"
