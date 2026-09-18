# case with no match has status 0; case status is the last command's
case x in y) false ;; esac; echo $?
case x in x) (exit 3) ;; esac; echo $?
