# ': ${VAR:=default}' sets defaults (git-sh-setup / mergetool style)
: ${QUILT_PATCHES:=patches}
: ${QUILT_SERIES:=$QUILT_PATCHES/series}
: "${LEVEL:="info"}"
echo "$QUILT_PATCHES $QUILT_SERIES $LEVEL"
