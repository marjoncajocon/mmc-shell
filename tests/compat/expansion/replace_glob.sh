# glob patterns in replacement: * ? and [ ]
s=abc123def456
echo "${s//[0-9]/#}"
echo "${s/[0-9]*/}"
echo "${s//?/.}"
