# GLOBIGNORE takes names out of glob results, and lets dot files in
tmp=$(mktemp -d); cd "$tmp" || exit 1
touch a.sh b.sh c.txt .hidden d.txt
echo *
GLOBIGNORE='*.sh'
echo *
GLOBIGNORE='*.sh:d*'
echo *
unset GLOBIGNORE
echo *
echo .*
cd / && rm -rf "$tmp"
