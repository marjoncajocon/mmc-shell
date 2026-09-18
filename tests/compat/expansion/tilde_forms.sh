# ~ and ~/x expand to $HOME (checked without printing it)
[ ~ = "$HOME" ] && echo ok1
[ ~/x = "$HOME/x" ] && echo ok2
[ "~" != "$HOME" ] && echo ok3
