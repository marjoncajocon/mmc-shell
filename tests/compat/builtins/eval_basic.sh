# eval re-parses its arguments
cmd='echo "hello from eval"'
eval "$cmd"
x=10
eval "y=\$x"
echo "$y"
eval 'for i in 1 2; do echo "i$i"; done'
