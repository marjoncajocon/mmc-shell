# usage() printed from a heredoc with the script name
usage() {
	cat <<EOF
usage: ${0##*/} [-v] file...
  -v   verbose
EOF
}
usage
