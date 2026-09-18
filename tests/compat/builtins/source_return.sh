# return in a sourced file stops the file and gives its status
tmp=$(mktemp -d)
printf 'echo part1\nreturn 4\necho part2\n' > "$tmp/r.sh"
. "$tmp/r.sh"
echo "status $?"
source "$tmp/missing.sh" 2>/dev/null
echo "missing status $?"
rm -rf "$tmp"
