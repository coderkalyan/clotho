// The C side of the exported-task svScope test in tests/dpi_test.cpp. An exported task
// takes its output by pointer and returns the int of LRM 35.9's disable protocol; an
// import task returns one the same way.

#include "svdpi.h"

// The task exported from several scopes, `task who(output int o)`.
extern int who(int* o);

// The handle svGetScopeFromName gives a hierarchical name.
void* c_scope(const char* name) {
    return svGetScopeFromName(name);
}

// Selects `scope` with svSetScope, runs the exported task there and hands back what it
// wrote. Reports a disable (a nonzero return of the exported task) as one of its own.
int c_who_task(void* scope, int* r) {
    int o = 0;
    svSetScope(scope);
    if (who(&o) != 0) {
        return 1;
    }
    *r = o;
    return 0;
}
