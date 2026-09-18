# forwarding "$@" keeps arguments intact
inner() { printf '<%s>\n' "$@"; }
wrap() { inner "$@"; }
wrap "a b" "" c
