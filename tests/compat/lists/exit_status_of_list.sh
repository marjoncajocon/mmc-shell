# status of a && list that stops early
f() { false && echo x; }
f; echo "status $?"
g() { true || echo x; }
g; echo "status $?"
