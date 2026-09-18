# if tests the exit status of any command list
if grep -q b <<< "abc"; then echo found; fi
if ! grep -q z <<< "abc"; then echo "not found"; fi
if true; false; then echo yes; else echo "last one counts"; fi
