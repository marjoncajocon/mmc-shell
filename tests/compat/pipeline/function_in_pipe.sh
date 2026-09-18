# functions can be used in pipelines
upper() { tr a-z A-Z; }
prefix() { while IFS= read -r l; do echo "> $l"; done; }
printf 'one\ntwo\n' | upper | prefix
