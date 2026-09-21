# shopt -s execfail: a script goes on when exec cannot run the program
shopt -s execfail
exec /no/such/program 2>/dev/null
echo "still here: $?"
