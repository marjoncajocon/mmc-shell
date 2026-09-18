# @Q @U @L @u @E transformations
s="it's a b"
echo "${s@Q}"
echo "${s@U}"
x='a\tb'
echo "${x@E}"
w=word
echo "${w@u}"
