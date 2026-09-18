# common string helpers: trim, lowercase, repeat, contains, starts-with
trim() { local s=$1; s=${s#"${s%%[![:space:]]*}"}; s=${s%"${s##*[![:space:]]}"}; printf '%s' "$s"; }
lower() { printf '%s' "${1,,}"; }
starts_with() { [[ $1 == "$2"* ]]; }
echo "[$(trim "   padded text  ")]"
echo "$(lower "MiXeD Case")"
starts_with "refs/heads/main" "refs/heads/" && echo "is a branch"
starts_with "v1.2" "refs/" || echo "not a ref"
s="a-b-c"
echo "${s//-/ }"
IFS=- read -r x y z <<< "$s"; echo "$z$y$x"
