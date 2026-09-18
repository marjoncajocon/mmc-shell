# read with a custom IFS for delimited fields
IFS=: read -r user pass uid rest <<< "root:x:0:0:root:/root:/bin/bash"
echo "$user $uid [$rest]"
IFS=, read -r a b c <<< "1,,3"
echo "[$a][$b][$c]"
