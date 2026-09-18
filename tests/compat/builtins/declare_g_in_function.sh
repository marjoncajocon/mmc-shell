# declare in a function is local unless -g
f() { declare loc=1; declare -g glob=2; }
f
echo "[${loc-unset}] [$glob]"
