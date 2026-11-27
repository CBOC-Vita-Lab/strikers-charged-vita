#include "NL/nlDebug.h"

#include "types.h"

#ifdef NP_NATIVE
#include "np/fatal.h"
#endif

void nlBreak()
{
#ifdef NP_NATIVE
    // NP (1.2): the reason a player's quiet build ends here is the caller; the platform keeps it in its basic log
    NpFatalNote("nlBreak", __builtin_return_address(0), 0);
#endif
    *(u32*)1 = 0;
}
