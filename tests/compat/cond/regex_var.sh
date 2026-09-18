# regex from a variable (must be unquoted to be a regex)
re='^(MINGW|MSYS|CYGWIN).*'
for os in MINGW64_NT-10.0 Linux MSYS_NT CYGWIN_NT; do
	if [[ $os =~ $re ]]; then echo "$os: windows (${BASH_REMATCH[1]})"; else echo "$os: other"; fi
done
[[ 'a.c' =~ "a.c" ]] && echo "quoted part literal"
[[ 'abc' =~ "a.c" ]] || echo "dot literal when quoted"
