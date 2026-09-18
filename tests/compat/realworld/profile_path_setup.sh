# /etc/profile style PATH building with ORIGINAL_PATH and MSYSTEM case
build_path() {
	local MSYSTEM=$1 ORIGINAL_PATH=$2 PATH
	case "${MSYSTEM}" in
		MINGW*|UCRT*|CLANG*)
			MINGW_MOUNT_POINT="/${MSYSTEM,,}"
			PATH="${MINGW_MOUNT_POINT}/bin:/usr/bin${ORIGINAL_PATH:+:${ORIGINAL_PATH}}"
			;;
		*)
			PATH="/usr/bin:/opt/bin${ORIGINAL_PATH:+:${ORIGINAL_PATH}}"
			;;
	esac
	echo "$PATH"
}
build_path MINGW64 "/c/Windows"
build_path MSYS ""
build_path CLANG64 "/c/a:/c/b"
