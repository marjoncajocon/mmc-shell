# >&2 and 1>&2 output is not on stdout
echo "to stdout"
echo "to stderr" >&2
echo "also stderr" 1>&2
>&2 echo "prefix form"
