# querying options with set -o / shopt
set -o pipefail
set -o | grep -E '^(pipefail|errexit|nounset)[[:space:]]'
shopt -s nullglob
shopt nullglob
shopt -p nullglob extglob
shopt -q nullglob && echo "nullglob on"
