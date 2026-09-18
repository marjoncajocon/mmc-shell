# quoted delimiter: no expansion at all
name=World
cat <<'EOF'
Hello, $name! $(echo no) `no` \n
EOF
cat <<"END"
$name
END
cat <<\EOF
$name too
EOF
