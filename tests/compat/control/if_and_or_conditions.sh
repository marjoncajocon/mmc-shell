# if with && and || in the condition
a=1 b=
if [ -n "$a" ] && [ -z "$b" ]; then echo both; fi
if [ -z "$a" ] || [ -z "$b" ]; then echo either; fi
if [ -n "$a" ] && { [ -n "$b" ] || [ "$a" = 1 ]; }; then echo grouped; fi
