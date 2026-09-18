# architecture normalisation (flutter update_dart_sdk style, mocked)
norm_arch() {
	case "$1" in
		x86_64|amd64) echo x64 ;;
		arm64|aarch64|armv8*) echo arm64 ;;
		i?86) echo ia32 ;;
		*) echo "unsupported: $1" ;;
	esac
}
for m in x86_64 amd64 aarch64 armv8l i686 riscv64; do norm_arch "$m"; done
