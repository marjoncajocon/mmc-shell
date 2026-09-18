# read splits into variables; the last gets the rest
read a b c <<< "one two three four five"
echo "a=$a b=$b c=$c"
read x <<< "  leading and trailing  "
echo "[$x]"
read <<< "into reply"
echo "[$REPLY]"
