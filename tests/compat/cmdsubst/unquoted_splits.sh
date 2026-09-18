# unquoted $( ) is split into words and globbed
count() { echo $#; }
count $(printf 'a b\nc')
count "$(printf 'a b\nc')"
