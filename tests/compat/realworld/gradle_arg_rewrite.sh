# gradlew style: rebuild "$@" with for arg do ... set -- "$@" "$arg"; shift; done
set -- one "two words" /c/path three
for arg do
	if case $arg in
		-*) false ;;
		/?*) true ;;
		*) false ;;
	esac
	then
		arg="CONV:$arg"
	fi
	shift
	set -- "$@" "$arg"
done
printf '<%s>\n' "$@"
