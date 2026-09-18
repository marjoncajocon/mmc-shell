# ; runs regardless; $? is the last command's status
false; echo $?
true; false; echo $?
