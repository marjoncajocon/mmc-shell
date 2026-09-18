# the loop variable keeps its last value; empty list leaves it unchanged
v=before
for v in a b c; do :; done
echo $v
w=keep
for w in; do :; done
echo $w
