# bash 5.2+: & in the replacement means the matched text (patsub_replacement)
s=abc
echo "${s/b/[&]}"
echo "${s/b/[\&]}"
