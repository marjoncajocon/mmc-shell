# replacing slashes: ${var//\//\\} and ${var//\//-}
p=/usr/local/bin
echo "${p//\//\\}"
echo "${p//\//-}"
w='C:\Users\me'
echo "${w//\\//}"
