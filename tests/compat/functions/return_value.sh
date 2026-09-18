# return N sets $?; plain return uses the last command's status
f() { return 3; }
g() { false; return; }
h() { true; }
f; echo $?
g; echo $?
h; echo $?
