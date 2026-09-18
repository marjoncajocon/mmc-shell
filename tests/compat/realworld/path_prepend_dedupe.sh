# add directories to PATH without duplicates
P=/usr/bin:/bin
add_path() {
	case ":$P:" in
		*":$1:"*) ;;
		*) P="$1${P:+:$P}" ;;
	esac
}
add_path /opt/tool/bin
add_path /usr/bin
add_path /opt/tool/bin
echo "$P"
IFS=: read -ra parts <<< "$P"
echo "${#parts[@]} entries"
