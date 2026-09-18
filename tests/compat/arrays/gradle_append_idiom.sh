# append with ${#arr[*]} as the index (gradle start script style)
JVM_OPTS=(-Xmx64m)
JVM_OPTS[${#JVM_OPTS[*]}]="-Dorg.appname=demo"
printf '%s\n' "${JVM_OPTS[@]}"
