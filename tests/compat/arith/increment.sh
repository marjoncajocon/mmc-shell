# pre/post increment and decrement, compound assignment
i=5
echo $((i++)) $i $((++i)) $i $((i--)) $((--i))
n=10
((n += 5)); echo $n
((n -= 3)); echo $n
((n *= 2)); echo $n
((n /= 4)); echo $n
((n %= 4)); echo $n
((n <<= 3)); echo $n
