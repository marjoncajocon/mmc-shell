# multi-stage pipeline
printf '%s\n' banana apple cherry apple | sort | awk '{print NR": "$0}' | tail -3 | head -2 | cut -d' ' -f2
