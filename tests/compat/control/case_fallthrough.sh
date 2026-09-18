# ;& falls through, ;;& tests the next pattern
case a in
	a) echo "matched a" ;&
	b) echo "fell into b" ;;
	c) echo "not here" ;;
esac
case abc in
	a*) echo "starts a" ;;&
	*c) echo "ends c" ;;&
	x*) echo "no" ;;
	*) echo "default too" ;;
esac
