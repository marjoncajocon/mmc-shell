# git-sh-setup style helpers: say, die_with_status, GIT_QUIET
say() { if test -z "$GIT_QUIET"; then printf '%s\n' "$*"; fi; }
die_with_status() { status=$1; shift; printf >&2 '%s\n' "$*"; exit "$status"; }
say "hello"
GIT_QUIET=1 say "quiet"
GIT_QUIET=1
say "also quiet"
unset GIT_QUIET
say "loud again"
( die_with_status 128 "fatal" ); echo "status $?"
