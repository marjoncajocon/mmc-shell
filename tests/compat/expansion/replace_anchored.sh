# ${v/#pat/rep} anchors at start, ${v/%pat/rep} at end
s=foo.foo
echo "${s/#foo/bar}"
echo "${s/%foo/bar}"
echo "${s/#oo/X}"
