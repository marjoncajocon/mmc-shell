# %b expands escapes in the argument; %q quotes for reuse
printf '%b\n' 'a\tb' 'c\\nd'
printf '%q\n' "a b" "it's" 'x$y' ''
