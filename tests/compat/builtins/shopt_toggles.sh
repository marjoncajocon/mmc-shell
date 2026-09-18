# shopt -s / -u / -q
shopt -q extglob && echo on || echo off
shopt -s extglob
shopt -q extglob && echo on || echo off
shopt -u extglob
shopt -q extglob && echo on || echo off
shopt -q no_such_opt 2>/dev/null; echo "status $?"
