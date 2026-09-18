# compgen -W filters a word list by prefix (completion scripts)
compgen -W "build bundle clean check" -- b
compgen -W "build clean" -- x; echo "status $?"
