# a case statement with ) patterns inside $( )
x=b
r=$(case $x in a) echo A ;; b) echo B ;; esac)
echo "$r"
