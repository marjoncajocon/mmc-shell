# case bodies with several commands and nested case
os=linux arch=arm64
case $os in
	linux)
		echo "os ok"
		case $arch in
			x86_64|amd64) echo x64 ;;
			arm64|aarch64) echo arm ;;
		esac
		;;
esac
