# patterns with [...] and ? in prefix/suffix removal
v=v1.2.3-rc1
echo "${v#[vV]}"
echo "${v%-rc?}"
echo "${v%%[.-]*}"
echo "${v##*[0-9].}"
