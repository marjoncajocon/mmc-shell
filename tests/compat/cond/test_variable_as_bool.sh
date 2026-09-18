# using true/false strings as booleans
verbose=true
if $verbose; then echo "verbose on"; fi
verbose=false
$verbose || echo "verbose off"
