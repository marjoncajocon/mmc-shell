# IFS= read -r keeps leading/trailing whitespace
IFS= read -r line <<< "   spaced out   "
echo "[$line]"
read -r line <<< "   spaced out   "
echo "[$line]"
