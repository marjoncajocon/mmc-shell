# $- reflects set options
set -eu
case $- in *e*) echo e ;; esac
case $- in *u*) echo u ;; esac
set +eu
case $- in *e*) echo still-e ;; *) echo "e off" ;; esac
