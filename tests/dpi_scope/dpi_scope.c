// The C side of the svScope tests in tests/dpi_test.cpp ("DPI scope: ..."). Every
// scope-dependent step is a small import the design drives, so the design's own
// $display is the whole transcript and nothing here prints.

#include <stddef.h>

#include "svdpi.h"

// The one subroutine exported from several scopes (LRM 35.4); each design exports it
// with its own body. Weak, because not every design that links this object exports it
// and an unresolved strong reference would fail the link.
extern int who(void) __attribute__((weak));

// The handle svGetScopeFromName gives a hierarchical name, NULL for an unknown one.
void* c_scope(const char* name) {
    return svGetScopeFromName(name);
}

// The name behind a handle, so the design can print which scope it got.
const char* c_name(void* scope) {
    return scope != NULL ? svGetNameFromScope(scope) : "(null)";
}

// The scope current while this import runs: the one the context import was declared
// in (LRM 35.5.3), unless svSetScope changed it.
const char* c_here(void) {
    return svGetNameFromScope(svGetScope());
}

// Calls the export in the scope the import itself runs in, with no svSetScope. The
// export may run context imports of other scopes: when it returns, the scope C was in
// is current again (LRM 35.5.3), or -1 says it is not.
int c_who_here(void) {
    const svScope started = svGetScope();
    const int     r       = who();
    return svGetScope() == started ? r : -1;
}

// Selects `scope` with svSetScope and calls the export there, with the same check.
int c_who_in(void* scope) {
    svSetScope(scope);
    const int r = who();
    return svGetScope() == scope ? r : -1;
}

// svSetScope returns the scope that was current: report whether it is the one the
// import started in, and whether svGetScope sees the new one afterwards.
int c_swap(void* scope) {
    const svScope started = svGetScope();
    const svScope prev    = svSetScope(scope);
    return (prev == started ? 1 : 0) + (svGetScope() == scope ? 2 : 0);
}

// Per-scope user data (LRM H.9.3): the same key holds a different value in each
// scope, and the scope a context import runs in is the one the data belongs to.
int c_count(void) {
    static int key;
    const svScope scope = svGetScope();
    long        n       = (long)svGetUserData(scope, &key);
    svPutUserData(scope, &key, (void*)(n + 1));
    return (int)(n + 1);
}
