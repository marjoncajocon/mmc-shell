# 64-bit signed integer arithmetic and overflow wraparound
echo $((2 ** 62)) $((9223372036854775807)) $((9223372036854775807 + 1))
echo $((1 << 63))
