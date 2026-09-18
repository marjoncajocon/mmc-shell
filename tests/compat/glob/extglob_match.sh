# extglob patterns in [[ ]] and case and ${var//}
shopt -s extglob
for v in 123 12a "" 007; do
	[[ $v == +([0-9]) ]] && echo "$v: number" || echo "$v: not number"
done
s="  trim me  "
t=${s##+([[:space:]])}
t=${t%%+([[:space:]])}
echo "[$t]"
case foo.tar.gz in *.@(gz|bz2)) echo compressed ;; esac
