# optional leading ( before patterns (gradlew style)
n=2
case $n in
	(0) echo zero ;;
	(1|2) echo "one or two" ;;
	(*) echo many ;;
esac
