# pre-push hook style: while read local_ref local_oid remote_ref remote_oid
zero=$(printf '%040d' 0)
while read -r local_ref local_oid remote_ref remote_oid; do
	if [ "$local_oid" = "$zero" ]; then
		echo "delete $remote_ref"
	elif [ "$remote_oid" = "$zero" ]; then
		echo "new branch $remote_ref"
	else
		echo "update $remote_ref ${remote_oid:0:7}..${local_oid:0:7}"
	fi
done <<EOF
refs/heads/main 1111111111111111111111111111111111111111 refs/heads/main 2222222222222222222222222222222222222222
refs/heads/feat 3333333333333333333333333333333333333333 refs/heads/feat 0000000000000000000000000000000000000000
(delete) 0000000000000000000000000000000000000000 refs/heads/old 4444444444444444444444444444444444444444
EOF
