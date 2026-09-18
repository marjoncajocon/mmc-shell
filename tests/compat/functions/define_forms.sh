# the three ways to define a function
f1() { echo f1; }
function f2 { echo f2; }
function f3() { echo f3; }
f4()
{
	echo f4
}
f1; f2; f3; f4
