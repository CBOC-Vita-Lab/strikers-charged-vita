/* Support for the generated SDK link stubs (platform/stubs/sdk_stubs.c). */
#ifndef NP_STUB_SUPPORT_H
#define NP_STUB_SUPPORT_H
#define NP_WEAK __attribute__((weak))
#ifdef __cplusplus
extern "C" {
#endif
void np_stub_report(const char *name);
void np_stub_vcall(void);                 /* target of the slots of stubbed vtables: reports and aborts */   /* logs once per stub; aborts if NP_STUB_ABORT=1 */
#ifdef __cplusplus
}
#endif
#define NP_STUB_HIT(name) do { static int np_hit_; if (!np_hit_) { np_hit_ = 1; np_stub_report(name); } else if (np_stub_abort_all()) abort(); } while (0)
int np_stub_abort_all(void);
#endif
