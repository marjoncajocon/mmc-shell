# dotfiles are matched only explicitly or with dotglob
tmp=$(mktemp -d); cd "$tmp" || exit 1
touch .env visible
echo *
echo .*[!.]*
shopt -s dotglob
echo *
cd / && rm -rf "$tmp"
