# discarding output with >/dev/null and 2>/dev/null
echo hidden > /dev/null
cat /no/such/file 2>/dev/null
echo "status $?"
echo shown
