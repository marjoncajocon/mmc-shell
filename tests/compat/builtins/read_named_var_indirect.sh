# read into a variable whose name is in a parameter (__git_eread style)
eread() { test -r "$1" && IFS=$'\r\n' read -r "$2" < "$1"; }
tmp=$(mktemp -d)
printf 'refs/heads/main\n' > "$tmp/HEAD"
eread "$tmp/HEAD" head && echo "head=$head"
eread "$tmp/none" other || echo "missing file: status $?"
rm -rf "$tmp"
