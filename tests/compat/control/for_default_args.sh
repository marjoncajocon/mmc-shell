# for without "in" iterates over "$@"
set -- x "y z"
for a; do echo "<$a>"; done
for a do echo "[$a]"; done
