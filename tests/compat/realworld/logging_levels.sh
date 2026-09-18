# log with levels and a verbosity threshold
LOG_LEVEL=${LOG_LEVEL:-2}
declare -A LEVELS=([debug]=3 [info]=2 [warn]=1 [error]=0)
log() {
	local lvl=$1; shift
	(( ${LEVELS[$lvl]} <= LOG_LEVEL )) || return 0
	printf '%-5s %s\n' "${lvl^^}" "$*"
}
log debug "hidden"
log info "starting"
log warn "careful"
log error "boom"
LOG_LEVEL=3 log debug "now visible"
