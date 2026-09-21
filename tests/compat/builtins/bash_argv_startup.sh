# BASH_ARGV / BASH_ARGC get the script's arguments when first used outside a function
f() { echo "in f: [${BASH_ARGV[*]}] [${BASH_ARGC[*]}]"; }
f one
echo "top: [${BASH_ARGV[*]}] [${BASH_ARGC[*]}]"
f two
set -- p q r
echo "after set: [${BASH_ARGV[*]}] [${BASH_ARGC[*]}]"
shopt -s extdebug
f x y
echo "extdebug top: [${BASH_ARGV[*]}] [${BASH_ARGC[*]}]"
