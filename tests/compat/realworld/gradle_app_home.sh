# gradlew style: APP_HOME=$( cd -P "${APP_HOME:-./}.." > /dev/null && printf '%s\n' "$PWD" )
tmp=$(mktemp -d)
mkdir -p "$tmp/proj/gradle/wrapper"
cd "$tmp/proj/gradle" || exit 1
app_path=$0
APP_BASE_NAME=${0##*/}
APP_HOME=$( cd -P "${APP_HOME:-./}.." > /dev/null && printf '%s\n' "$PWD" ) || exit
echo "base=$APP_BASE_NAME home=${APP_HOME##*/}"
cd / && rm -rf "$tmp"
