# <(cmd) gives a readable file name
cat <(echo from-procsub)
diff_like() { while IFS= read -r l; do echo "got $l"; done < "$1"; }
diff_like <(printf 'x\ny\n')
