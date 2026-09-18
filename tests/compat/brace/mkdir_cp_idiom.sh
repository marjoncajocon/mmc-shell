# mkdir -p dir/{a,b} and backup idiom
tmp=$(mktemp -d)
mkdir -p "$tmp"/proj/{src,test,doc}
ls "$tmp/proj"
: backup idiom would be cp f.conf{,.bak}
touch "$tmp"/f.conf{,.bak}
ls "$tmp" | grep conf
rm -rf "$tmp"
