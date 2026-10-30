// The C side of tests/dpi_shadow/shadow.sv: a definition of a name libc also
// defines. A --dpi-lib definition must win over the process's own symbol.

int getpid(void) {
    return 31337;
}
