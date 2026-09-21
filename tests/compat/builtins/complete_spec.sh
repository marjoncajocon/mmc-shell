# complete: storing, printing and removing a completion rule
complete -W "start stop restart" svc
complete -p svc
complete -F _handler -o nospace -o filenames deploy
complete -p deploy
complete -r svc
complete -p svc 2>/dev/null || echo "gone: $?"
complete -W "x y" a1 a2
complete -p a1
complete -p a2
complete -r
complete -p 2>/dev/null | wc -l
