# && and || at end of line continue the list
true &&
	echo cont1 ||
	echo never
false ||
	echo cont2
