# declare -i makes assignments evaluate arithmetic
declare -i n
n=3+4
echo $n
n+=10
echo $n
n="n * 2"
echo $n
