# ${v%pat} shortest, ${v%%pat} longest suffix removal
f=archive.tar.gz
echo "${f%.*}"
echo "${f%%.*}"
d=/a/b/c/
echo "${d%/}"
p=/a/b/c
echo "${p%/*}"
