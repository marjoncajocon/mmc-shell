# basename/dirname/extension with parameter expansion vs commands
f=/home/user/src/project/main.test.js
echo "${f##*/} | $(basename "$f")"
echo "${f%/*} | $(dirname "$f")"
name=${f##*/}
echo "stem=${name%%.*} ext=${name##*.} base=${name%.*}"
echo "$(basename "$f" .js)"
echo "$(dirname relative)" "$(dirname /)" "$(dirname a/b/)"
