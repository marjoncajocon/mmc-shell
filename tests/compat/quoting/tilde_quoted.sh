# tilde is only expanded unquoted at the start of a word
echo "~" '~' x~y
a=~
[ "$a" = "$HOME" ] && echo tilde-expanded-in-assignment
