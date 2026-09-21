# shopt compat31..compat44 are one level at a time, and BASH_COMPAT follows them (and sets them)
echo "start: ${BASH_COMPAT-unset}"
shopt -s compat42; echo "compat42: $BASH_COMPAT"; shopt compat31 compat42 compat43
shopt -s compat31; echo "compat31: $BASH_COMPAT"; shopt compat31 compat42
shopt -u compat42; echo "unset other: $BASH_COMPAT"; shopt compat31
shopt -u compat31; shopt | grep -c 'compat.*on'
BASH_COMPAT=4.3; shopt compat43; echo "$BASH_COMPAT"
BASH_COMPAT=44; shopt compat43 compat44
BASH_COMPAT=3.2; shopt compat32
BASH_COMPAT=50; shopt | grep -c 'compat.*on'
BASH_COMPAT=41; unset BASH_COMPAT; shopt compat41; echo "${BASH_COMPAT-unset}"
BASH_COMPAT=40; BASH_COMPAT=; shopt compat40
BASH_COMPAT=42; BASH_COMPAT=99 2>/dev/null; echo "bad: $BASH_COMPAT"; shopt compat42
BASH_COMPAT=30 2>/dev/null; shopt | grep -c 'compat.*on'
echo "status $?"
