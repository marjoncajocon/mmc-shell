# caller with and without a frame number; BASH_ARGV and BASH_ARGC with extdebug
f() { g x y; }
g() {
  caller
  caller 0
  caller 1
  caller 2; echo "past the top: $?"
  echo "argc=${BASH_ARGC[*]} argv=${BASH_ARGV[*]}"
}
f a b c
shopt -s extdebug
f a b c
shopt -u extdebug
caller; echo "outside a function: $?"
