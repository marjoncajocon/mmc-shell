# retry with a countdown (flutter pub_upgrade_with_retry style, no sleep)
attempts=0
flaky() { attempts=$((attempts + 1)); [ $attempts -ge 3 ]; }
retry() {
	local total_tries=5
	local remaining_tries=$((total_tries - 1))
	while [[ "$remaining_tries" -gt 0 ]]; do
		"$@" && break
		echo "failed, retrying ($remaining_tries tries left)"
		remaining_tries=$((remaining_tries - 1))
	done
	if [[ "$remaining_tries" == 0 ]]; then echo "giving up"; return 1; fi
	return 0
}
retry flaky && echo "succeeded after $attempts attempts"
attempts=-10
retry flaky || echo "retry status $?"
