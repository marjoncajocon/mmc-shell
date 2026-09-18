# an unquoted ${v:-a b} result is split into words
count() { echo $#; }
count ${unset_v:-a b c}
count "${unset_v:-a b c}"
