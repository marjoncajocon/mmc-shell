# comparison and logical operators return 1 or 0
echo $((3 < 5)) $((3 > 5)) $((3 <= 3)) $((3 >= 4)) $((3 == 3)) $((3 != 3))
echo $((1 && 0)) $((1 || 0)) $((!5)) $((!0))
