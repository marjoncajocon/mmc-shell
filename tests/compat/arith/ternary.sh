# ?: ternary operator, including nested
a=7
echo $((a > 5 ? 1 : 0)) $((a > 10 ? 100 : a > 5 ? 50 : 0))
