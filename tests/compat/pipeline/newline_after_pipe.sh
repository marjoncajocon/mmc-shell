# a line may end with | and continue on the next
echo hello |
	tr a-z A-Z |
	sed 's/L/_/g'
