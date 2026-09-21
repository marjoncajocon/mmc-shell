# compat43: break in a function ends the caller's loop; compat44: break in ( ) leaves the subshell
f() { break; }
g() { continue; }
h() { for j in a b; do break 2; done; echo "h after"; }
for lvl in 43 44 52; do
  BASH_COMPAT=$lvl
  echo "== $lvl"
  for i in 1 2 3; do f 2>/dev/null; echo "f $i"; done
  for i in 1 2; do g 2>/dev/null; echo "g $i"; done
  for i in 1 2; do h 2>/dev/null; echo "h $i"; done
  for i in 1 2; do (break; echo "sub $i") 2>/dev/null; echo "loop $i"; done
  for i in 1 2; do (continue; echo "csub $i") 2>/dev/null; echo "cloop $i"; done
  for i in 1 2; do x=$(break; echo in); echo "cs $i [$x]"; done
done
