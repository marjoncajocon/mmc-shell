# ! inverts the status of a pipeline
! false; echo $?
! true; echo $?
! echo x | grep -q y; echo $?
