# $LINENO reports the current line
echo "line $LINENO"
f() {
	echo "in f line $LINENO"
}
f
