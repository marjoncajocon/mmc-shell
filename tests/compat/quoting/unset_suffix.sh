# $var_suffix reads a different variable; ${var}_suffix does not
var=abc
echo "[$var_suffix]"
echo "[${var}_suffix]"
echo "[$var.suffix]"
