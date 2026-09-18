# read CSV rows with IFS=, and sum a column
tmp=$(mktemp -d)
printf 'name,qty,price\napple,3,2\npear,5,1\nfig,2,7\n' > "$tmp/data.csv"
total=0
{
	read -r _header
	while IFS=, read -r name qty price; do
		line=$((qty * price))
		total=$((total + line))
		printf '%-6s %3d\n' "$name" "$line"
	done
} < "$tmp/data.csv"
echo "total $total"
rm -rf "$tmp"
