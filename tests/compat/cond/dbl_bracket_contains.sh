# [[ $x == *"$y"* ]] substring test (flutter/git style)
v="git version 2.45.1 (Apple Git-154)"
[[ $v == *"Apple Git"* ]] && echo apple
[[ $v == *windows* ]] || echo "not windows"
needle="2.45"
[[ $v == *"$needle"* ]] && echo "has $needle"
