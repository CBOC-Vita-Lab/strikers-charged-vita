/* NPP: symbols that the -O2 objects reference and the checked-in stub file (generated from -O1 objects) lacks. Linked only into the
 * perf variants (tools/perf/build.sh, LDEXTRA); they are decomp gaps (NPG writes the real ones): behave like the generated stubs
 * (return 0, no output). WEAK: a real definition (NPG merged one of them) wins. */
void np_perf_stub_uv18(void) __asm__("_ZN11WorldObject21UnidentifiedVirtual18EP5World");
__attribute__((weak)) void np_perf_stub_uv18(void) {}
int np_perf_stub_cfff(void) __asm__("_Z24CallFielderFuzzyFunctionPvRKjP8cFielder");
__attribute__((weak)) int np_perf_stub_cfff(void) { return 0; }
