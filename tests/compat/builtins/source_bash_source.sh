# BASH_SOURCE inside a sourced file names that file
tmp=$(mktemp -d)
echo 'echo "sourced: ${BASH_SOURCE[0]##*/} from ${BASH_SOURCE[1]##*/}"' > "$tmp/inner.sh"
. "$tmp/inner.sh"
echo "main: ${BASH_SOURCE[0]##*/}"
rm -rf "$tmp"
