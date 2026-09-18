# comma operator evaluates left to right, value of the last
echo $((a = 2, b = a * 3, a + b))
echo "$a $b"
