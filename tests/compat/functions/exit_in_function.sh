# exit inside a function ends the whole script
f() { echo "before exit"; exit 3; echo never; }
f
echo "not reached"
