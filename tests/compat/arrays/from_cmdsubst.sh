# array from command output split on IFS
a=($(printf '%s\n' x y z))
echo "${#a[@]} ${a[1]}"
IFS=$'\n'
b=($(printf 'one two\nthree\n'))
echo "${#b[@]} [${b[0]}]"
