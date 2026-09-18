# ${v:+word} gives word only when v is set and non-empty
unset u; e=; s=x
echo "[${u:+alt}] [${e:+alt}] [${s:+alt}]"
echo "[${u+alt}] [${e+alt}] [${s+alt}]"
