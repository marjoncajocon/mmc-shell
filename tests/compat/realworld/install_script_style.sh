# curl|bash installer structure without network: detect, choose, confirm
set -e
PREFIX=${PREFIX:-/usr/local}
INSTALL_DIR="${PREFIX%/}/lib/tool"
say() { printf 'tool-install: %s\n' "$*"; }
err() { say "$*" >&2; exit 1; }
platform() {
	local os=$1 arch=$2
	case "$os-$arch" in
		Linux-x86_64) echo linux-x64 ;;
		Darwin-arm64) echo darwin-arm64 ;;
		MINGW*-x86_64|MSYS*-x86_64) echo windows-x64 ;;
		*) return 1 ;;
	esac
}
for combo in "Linux x86_64" "Darwin arm64" "MSYS_NT x86_64" "SunOS sparc"; do
	set -- $combo
	if p=$(platform "$1" "$2"); then say "download tool-$p.tar.gz to $INSTALL_DIR"; else say "unsupported $1/$2"; fi
done
yes_flag=${YES:-n}
case $yes_flag in [yY]*) say "auto-confirm" ;; *) say "would ask for confirmation" ;; esac
