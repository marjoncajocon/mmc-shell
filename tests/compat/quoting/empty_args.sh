# "" and '' are real (empty) arguments, unquoted empty var vanishes
e=
count() { echo $#; }
count "" '' $e "$e"
count $e $e
