# $'...' ANSI-C quoting: \t \n \\ \' \x41 \101 \e is not printed
printf '[%s]\n' $'a\tb' $'line1\nline2' $'back\\slash' $'q\'s' $'\x41\101'
