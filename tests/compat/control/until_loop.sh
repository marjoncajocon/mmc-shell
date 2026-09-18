# until loop runs while the condition fails
i=0
until [ $i -ge 3 ]; do echo "u$i"; i=$((i + 1)); done
