# builtin runs only builtins
builtin printf '%s\n' ok
builtin no_such_builtin 2>/dev/null; echo "status $?"
