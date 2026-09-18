# OS detection with case "$(uname -s)" (value never printed; mocked too)
detect() {
	case "$1" in
		MINGW*|MSYS*|CYGWIN*) echo windows ;;
		Linux*) echo linux ;;
		Darwin*) echo macos ;;
		*) echo unknown ;;
	esac
}
for s in MINGW64_NT-10.0-26200 MSYS_NT-10.0 CYGWIN_NT-10.0 Linux Darwin FreeBSD; do
	echo "$s -> $(detect "$s")"
done
os=$(detect "$(uname -s)")
[ -n "$os" ] && echo "real os detected"
