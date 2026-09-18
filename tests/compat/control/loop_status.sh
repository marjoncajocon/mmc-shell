# status of a loop is the status of the last command run in its body
for i in 1 2; do false; done; echo $?
for i in; do false; done; echo $?
while false; do :; done; echo $?
