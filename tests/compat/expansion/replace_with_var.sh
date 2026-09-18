# pattern and replacement can come from variables
s="hello world"
from=world
to="there  friend"
echo "${s/$from/$to}"
echo "${s/"$from"/X}"
