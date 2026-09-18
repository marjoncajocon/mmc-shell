# ${var:+:$var} to join path lists without a stray colon
ORIG=
P="/bin${ORIG:+:${ORIG}}"
echo "$P"
ORIG=/usr/local/bin
P="/bin${ORIG:+:${ORIG}}"
echo "$P"
