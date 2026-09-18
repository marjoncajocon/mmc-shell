# shift, shift N and shifting too far
set -- 1 2 3 4 5
shift; echo "$*"
shift 2; echo "$*"
shift 5; echo "status $? args $#"
shift 2; echo "$*"
shift; echo "status $?"
