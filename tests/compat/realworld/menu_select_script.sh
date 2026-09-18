# a select menu driven by piped input
choose() {
	local PS3="pick: "
	select opt in build test quit; do
		case $opt in
			build|test) echo "doing $opt" ;;
			quit) echo "bye"; break ;;
			*) echo "invalid $REPLY" ;;
		esac
	done
}
printf '1\n5\n2\n3\n' | choose
