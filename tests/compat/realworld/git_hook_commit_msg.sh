# commit-msg hook style: check a message file with grep/sed
tmp=$(mktemp -d)
printf 'feat: add thing\n\nSigned-off-by: A <a@x>\nSigned-off-by: A <a@x>\n' > "$tmp/MSG"
dups=$(grep '^Signed-off-by: ' "$tmp/MSG" | sort | awk 'seen[$0]++ == 1')
[ -n "$dups" ] && echo "duplicate sign-off: $dups"
head -1 "$tmp/MSG" | grep -qE '^(feat|fix|docs): ' && echo "subject ok"
rm -rf "$tmp"
