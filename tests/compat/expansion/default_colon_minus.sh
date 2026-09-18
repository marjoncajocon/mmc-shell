# ${v:-word} uses word when v is unset or empty
unset u; e=; s=set
echo "${u:-def} ${e:-def} ${s:-def}"
echo "${u-def} [${e-def}] ${s-def}"
