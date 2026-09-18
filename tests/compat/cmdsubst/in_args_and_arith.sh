# substitutions in arithmetic and in for lists
n=$(( $(echo 3) * 2 ))
echo $n
for w in $(echo x y z); do printf '%s-' "$w"; done; echo
