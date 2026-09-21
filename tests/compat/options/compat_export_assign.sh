# v=1 export v / readonly v keep the value; compat44 also gives it to the global when v is a local
run() {
  (
    v=g
    f() { local v=0; v=1 export v; echo "f: $v"; declare -p v; }
    f; echo "after f: ${v-unset}"
    unset v
    k() { v=1 export v; w=2 readonly w; }
    k; echo "k: v=${v-unset} w=${w-unset}"; declare -p v w
    u=0; u=5 export -n u; declare -p u
    t=0; t=6 export t=7; declare -p t
    q=0; q=9 echo -n; echo "q=$q"
  )
}
run
BASH_COMPAT=44
run
s=0; s=8 declare -x s; declare -p s
p=0; p=3 readonly p; declare -p p
