# classic argument parsing loop: flags, --opt=value, --opt value, --, positionals
main() {
	verbose=0 output= target=release
	files=()
	while [ $# -gt 0 ]; do
		case "$1" in
			-h|--help) echo "usage: build [-v] [-o FILE] [--target=T] files..."; return 0 ;;
			-v|--verbose) verbose=$((verbose + 1)) ;;
			-o|--output) output=$2; shift ;;
			--output=*) output=${1#*=} ;;
			--target=*) target=${1#--target=} ;;
			--) shift; files+=("$@"); break ;;
			-*) echo "unknown option: $1" >&2; return 2 ;;
			*) files+=("$1") ;;
		esac
		shift
	done
	echo "verbose=$verbose output=${output:-none} target=$target files=${#files[@]}"
	printf '  file: %s\n' "${files[@]}"
}
main -v -v -o out.bin --target=debug a.c "b c.c" -- -weird.c
main --output=x.o z.c
main --help
main --bogus; echo "status $?"
