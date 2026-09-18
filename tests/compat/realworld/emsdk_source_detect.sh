# emsdk_env.sh style: detect the sourced script path with BASH_SOURCE
CURRENT_SCRIPT=
DIR="."
if [ -n "${BASH_SOURCE-}" ]; then
	CURRENT_SCRIPT="$BASH_SOURCE"
elif [ -n "${ZSH_VERSION-}" ]; then
	CURRENT_SCRIPT="zsh"
fi
if [ -n "${CURRENT_SCRIPT-}" ]; then
	DIR=$(dirname "$CURRENT_SCRIPT")
fi
unset CURRENT_SCRIPT
echo "DIR=$DIR"
echo "[${CURRENT_SCRIPT-unset}]"
