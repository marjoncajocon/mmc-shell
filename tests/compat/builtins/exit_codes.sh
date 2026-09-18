# exit status conventions: 127 not found, exit N masks to 0-255
no_such_command_xyz 2>/dev/null; echo $?
( exit 256 ); echo $?
( exit 257 ); echo $?
( exit -1 ); echo $?
