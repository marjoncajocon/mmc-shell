# (( )) returns status 0 for non-zero result, 1 for zero
((5 > 3)); echo $?
((0)); echo $?
((1 - 1)); echo $?
x=0
if ((x)); then echo yes; else echo no; fi
