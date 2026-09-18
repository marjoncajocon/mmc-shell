# mapfile/readarray reads lines into an array
mapfile -t lines <<EOF
first line
second line

fourth
EOF
echo "${#lines[@]}"
printf '[%s]\n' "${lines[@]}"
readarray -t nums < <(seq 3)
echo "${nums[*]}"
