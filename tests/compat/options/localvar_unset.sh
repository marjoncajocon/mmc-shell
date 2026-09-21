# unset of a caller's local: shows the global by default, stays unset with shopt -s localvar_unset
x=global
inner() { unset x; echo "inner: ${x-UNSET}"; x=from_inner; }
outer() { local x=outer_local; inner; echo "outer: ${x-UNSET}"; }
same() { local x=same_local; unset x; echo "same: ${x-UNSET}"; x=again; echo "same again: $x"; }
outer; echo "global: $x"
x=global
same; echo "global: $x"
shopt -s localvar_unset
x=global
outer; echo "global: $x"
same; echo "global: $x"
deep() { unset x; echo "deep: ${x-UNSET}"; }
mid() { deep; echo "mid: ${x-UNSET}"; }
top() { local x=top_local; mid; echo "top: ${x-UNSET}"; x=reset; echo "top again: $x"; }
top; echo "global: $x"
shopt -u localvar_unset
top; echo "global: $x"
twice() { unset x; unset x; echo "twice: ${x-UNSET}"; }
owner() { local x=1; twice; echo "owner: ${x-UNSET}"; }
x=global
owner; echo "global: ${x-UNSET}"
