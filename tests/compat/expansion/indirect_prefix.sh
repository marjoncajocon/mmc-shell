# ${!prefix@} lists variable names with a prefix
APP_ONE=1 APP_TWO=2 APP_THREE=3
for v in "${!APP_@}"; do echo "$v=${!v}"; done
