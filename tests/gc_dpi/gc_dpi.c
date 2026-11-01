// The C side of tests/gc_dpi/gc_dpi.sv: imports that call back into exported SV
// subroutines which allocate class objects.

extern int  sv_churn(int n);
extern int  sv_hold(int n);
extern void sv_task_hold(int n);

int c_churn(int n) {
    return sv_churn(n);
}

int c_hold(int n) {
    return sv_hold(n);
}

int c_task_hold(int n) {
    sv_task_hold(n);
    return 0;
}
