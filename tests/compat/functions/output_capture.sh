# returning strings by echo and $( )
join_by() { local IFS=$1; shift; echo "$*"; }
r=$(join_by , a b c)
echo "$r"
echo "$(join_by / usr local bin)"
