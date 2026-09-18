# set -- $var splits a string into positionals (git-sh-setup style)
line="deadbeef commit refs/heads/main"
set -- $line
echo "sha=$1 type=$2 ref=$3"
IFS=/ ; set -- $3; unset IFS
echo "$# parts, last=$3"
