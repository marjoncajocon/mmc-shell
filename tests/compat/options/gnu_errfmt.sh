# shopt -s gnu_errfmt: shell errors read "file:line: msg"; a builtin's own keep "line N"
{
  nosuch_command_q1
  shopt -s gnu_errfmt
  nosuch_command_q2
  cd ./no_such_dir_q3
  (r=1; readonly r; r=2)
  shopt -u gnu_errfmt
  nosuch_command_q4
} 2>&1
echo "done"
