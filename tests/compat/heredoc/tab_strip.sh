# <<- strips leading tabs (not spaces) from lines and delimiter
if true; then
	cat <<-EOF
		tabbed line
	  tab then spaces
	EOF
fi
