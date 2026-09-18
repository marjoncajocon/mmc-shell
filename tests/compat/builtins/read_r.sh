# read without -r treats backslashes as escapes
read v <<< 'a\tb\\c\ d'
echo "[$v]"
read -r v <<< 'a\tb\\c\ d'
echo "[$v]"
