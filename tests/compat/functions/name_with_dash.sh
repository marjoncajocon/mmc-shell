# function names with - : and . (git style names)
git-sh-say() { echo "say: $*"; }
ns::helper() { echo "ns helper"; }
my.func() { echo "dotted"; }
git-sh-say hi
ns::helper
my.func
