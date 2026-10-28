// The C side of tests/exe/dpi.sv.

extern int sv_twice(int x);

int c_add(int a, int b) {
    return a + b;
}

int c_calls_back(int x) {
    return sv_twice(x) + 1;
}
