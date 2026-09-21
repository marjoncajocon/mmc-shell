# compat31: a quoted =~ right side is still a regex; compat42: no quote removal in "${x/p/r}"
check() {
  [[ abc =~ "a.c" ]] && echo "dq yes" || echo "dq no"
  [[ abc =~ 'a.c' ]] && echo "sq yes" || echo "sq no"
  [[ abc =~ a\.c ]] && echo "bs yes" || echo "bs no"
  p='a.c'
  [[ abc =~ "$p" ]] && echo "var yes" || echo "var no"
  [[ abc =~ $p ]] && echo "bare var yes" || echo "bare var no"
  x=abc
  echo "1 ${x/b/'X'}"
  echo "2 ${x/b/\"X\"}"
  echo "3 ${x/b/"X"}"
  echo 4 ${x/b/'X'}
  echo "5 ${x//b/\\}"
  echo "6 ${x/b/\Q}"
  echo "7 ${x/b/&}" "${x/b/[&]}" "${x/b/\&}"
  r='<&>'
  echo "8 ${x/b/$r}" "${x/b/"$r"}"
}
for lvl in 31 32 42 43 52; do
  BASH_COMPAT=$lvl
  echo "== $lvl"
  check
done
