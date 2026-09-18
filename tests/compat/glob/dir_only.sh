# a trailing slash matches only directories
tmp=$(mktemp -d); cd "$tmp" || exit 1
mkdir d1 d2; touch f1
echo */
echo d*/
cd / && rm -rf "$tmp"
