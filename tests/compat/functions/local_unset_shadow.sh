# local without value hides the global inside the function
v=global
f() { local v; echo "[${v-unset}]"; v=inner; echo "[$v]"; }
f
echo "[$v]"
