#include <stdio.h>
#include <stdlib.h>
#include "np_stub_support.h"

int np_stub_abort_all(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("NP_STUB_ABORT"); v = (e && *e && *e != '0'); }
    return v;
}

void np_stub_report(const char *name)
{
    fprintf(stderr, "NP STUB: %s\n", name);
    fflush(stderr);
    if (np_stub_abort_all()) abort();
}

void np_stub_vcall(void)
{
    fprintf(stderr, "NP STUB: a virtual function of a class whose vtable is a gap in the decomp was called\n");
    fflush(stderr);
    abort();
}
