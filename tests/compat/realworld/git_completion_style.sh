# git-completion style: __gitcomp-like filtering with case on "$cur_"
gitcomp() {
	local cur_="${3-$cur}" c
	case "$cur_" in
		--*=) ;;
		*)
			for c in $1; do
				c="$c${4-}"
				if [[ $c == "$cur_"* ]]; then
					case $c in --*=|*.) ;; *) c="$c " ;; esac
					COMPREPLY[${#COMPREPLY[@]}]="${2-}$c"
				fi
			done
			;;
	esac
}
cur=--st
COMPREPLY=()
gitcomp "--stat --stage --patch --summary"
printf '[%s]\n' "${COMPREPLY[@]}"
COMPREPLY=()
gitcomp "main master dev" "" "ma"
printf '[%s]\n' "${COMPREPLY[@]}"
