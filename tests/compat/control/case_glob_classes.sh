# character classes and prefix/suffix patterns in case
for f in main.c lib.h README Makefile x.tar.gz 9lives; do
	case $f in
		*.c|*.h) echo "$f: source" ;;
		*.tar.*) echo "$f: archive" ;;
		[[:upper:]]*) echo "$f: capital" ;;
		[[:digit:]]*) echo "$f: digit" ;;
	esac
done
