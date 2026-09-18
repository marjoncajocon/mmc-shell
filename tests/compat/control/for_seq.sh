# for over $(seq ...) and brace ranges
for i in $(seq 1 3); do printf '%s ' "$i"; done; echo
for i in {1..3}; do printf '%s ' "$i"; done; echo
