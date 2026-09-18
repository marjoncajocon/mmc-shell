# export NAME=value, export existing, export -n
A=1
export A
export B=2 C=3
env | grep -E '^[ABC]=' | sort
export -n B
env | grep -E '^[ABC]=' | sort
