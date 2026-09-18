# check if a value is in an array (common helper)
contains() { local x=$1; shift; local e; for e; do [ "$e" = "$x" ] && return 0; done; return 1; }
list=(foo "bar baz" qux)
contains "bar baz" "${list[@]}" && echo yes
contains bar "${list[@]}" || echo no
