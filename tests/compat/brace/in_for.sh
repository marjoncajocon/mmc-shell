# brace expansion as a for list
for f in file{1..3}.txt; do printf '%s ' "$f"; done; echo
