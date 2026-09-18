# PIPESTATUS holds each command's status
true | false | (exit 3) | true
echo "${PIPESTATUS[@]}"
false
echo "${PIPESTATUS[@]}"
