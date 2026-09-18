# a redirect to an impossible path makes the command fail without running it
echo hi > /no/such/dir/file 2>/dev/null
echo "status $?"
