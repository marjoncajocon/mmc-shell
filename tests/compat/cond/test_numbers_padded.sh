# -eq compares numerically; = compares strings
[ 007 -eq 7 ] && echo "numeric equal"
[ 007 = 7 ] || echo "string differ"
[ -3 -lt 2 ] && echo "negative ok"
