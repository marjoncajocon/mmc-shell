# status of an if with no branch taken is 0; otherwise the branch status
if false; then :; fi; echo $?
if true; then (exit 4); fi; echo $?
