# echo -n, -e, -E and combined flags
echo -n "no newline"; echo "|"
echo -e "tab\there\nnew"
echo -E "raw\tstays"
echo "default\tno-escapes"
echo -ne "a\x41\101\n"
