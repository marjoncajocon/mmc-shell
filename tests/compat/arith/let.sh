# let evaluates each argument; status is from the last one
let a=5 b=a*2
echo "$a $b"
let "c = a + b"
echo $c
let z=0; echo $?
let z=1; echo $?
