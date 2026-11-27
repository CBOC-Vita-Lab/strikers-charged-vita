/* profrt_min.c: a minimal clang IR-PGO profile runtime (raw profile format version 9, LLVM 18), for hosts that have no libclang_rt.profile
 * (this sandbox: clang 18 ships none and the compiler-rt sources are not on disk). Covers what -fprofile-generate code needs: the section
 * bounds, __llvm_profile_runtime, the value-profile entry points (indirect calls, memop sizes) and __llvm_profile_write_file (the harness
 * calls it at exit). Build for the recording target (host32: clang -m32 -O2 -c) and pass the object in LDEXTRA with
 *   -Wl,-u,__llvm_profile_runtime -Wl,-u,__llvm_profile_write_file -Wl,-u,__llvm_profile_instrument_target
 * Output file: $LLVM_PROFILE_FILE (%p = pid), default default.profraw. The layout follows compiler-rt's InstrProfilingWriter: header, data
 * records, padding, 64-bit counters, bitmap bytes, names, then one ValueProfData per record that has value sites. Single process; counter
 * updates are not atomic (the usual compiler-rt behaviour); value-node insertion is a CAS. Check a recording with llvm-profdata-18 show. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int __llvm_profile_runtime = 0;
extern uint64_t __llvm_profile_raw_version __attribute__((weak));

typedef struct __attribute__((aligned(8))) {   /* INSTR_PROF_DATA, 32-bit pointers on the host32 target (IntPtrT = uintptr_t) */
  uint64_t NameRef, FuncHash; uintptr_t CounterPtr, BitmapPtr, FunctionPointer, Values;
  uint32_t NumCounters; uint16_t NumValueSites[2]; uint32_t NumBitmapBytes;
} Data;
typedef struct VNode { uint64_t Value, Count; struct VNode *Next; } VNode;

extern Data __start___llvm_prf_data[] __attribute__((weak, visibility("hidden"))), __stop___llvm_prf_data[] __attribute__((weak, visibility("hidden")));
extern char __start___llvm_prf_cnts[] __attribute__((weak, visibility("hidden"))), __stop___llvm_prf_cnts[] __attribute__((weak, visibility("hidden")));
extern char __start___llvm_prf_bits[] __attribute__((weak, visibility("hidden"))), __stop___llvm_prf_bits[] __attribute__((weak, visibility("hidden")));
extern char __start___llvm_prf_names[] __attribute__((weak, visibility("hidden"))), __stop___llvm_prf_names[] __attribute__((weak, visibility("hidden")));

static void instr(uint64_t v, void *d, uint32_t idx) {
  VNode **head = &((VNode **)((Data *)d)->Values)[idx];
  for (VNode *n = *head; n; n = n->Next) if (n->Value == v) { n->Count++; return; }
  VNode *n = (VNode *)calloc(1, sizeof *n); if (!n) return;
  n->Value = v; n->Count = 1;
  for (;;) { VNode *h = *head; n->Next = h; if (__sync_bool_compare_and_swap(head, h, n)) return; }
}
void __llvm_profile_instrument_target(uint64_t v, void *d, uint32_t idx) { instr(v, d, idx); }
void __llvm_profile_instrument_memop(uint64_t v, void *d, uint32_t idx) { instr(v, d, idx); }
void __llvm_profile_instrument_range(uint64_t v, void *d, uint32_t idx, int64_t lo, int64_t hi, int64_t large) {
  instr((v >= (uint64_t)lo && v <= (uint64_t)hi) ? v : (uint64_t)large, d, idx);
}

static const char zeros[8] = {0};
static void pad(FILE *f, size_t n) { size_t p = (8 - n % 8) % 8; if (p) fwrite(zeros, 1, p, f); }

int __llvm_profile_write_file(void) {
  const char *fn = getenv("LLVM_PROFILE_FILE"); char path[1024];
  if (!fn || !*fn) fn = "default.profraw";
  { size_t o = 0; for (; *fn && o < sizeof path - 16; fn++) { if (fn[0] == '%' && fn[1] == 'p') { o += (size_t)snprintf(path + o, 16, "%d", (int)getpid()); fn++; } else path[o++] = *fn; } path[o] = 0; }
  Data *db = __start___llvm_prf_data, *de = __stop___llvm_prf_data;
  size_t rawd = (size_t)((char *)de - (char *)db), nData = (rawd + sizeof(Data) - 1) / sizeof(Data), dsz = nData * sizeof(Data);   /* the last record of the section lacks its tail padding (44 of 48 bytes on i386) */
  size_t csz = (size_t)(__stop___llvm_prf_cnts - __start___llvm_prf_cnts), bsz = (size_t)(__stop___llvm_prf_bits - __start___llvm_prf_bits);
  size_t nsz = (size_t)(__stop___llvm_prf_names - __start___llvm_prf_names);
  FILE *f = fopen(path, "wb"); if (!f) return 1;
  uint64_t h[14] = { 0xff6c70726f665281ull /* lprofR, 32-bit */, __llvm_profile_raw_version ? __llvm_profile_raw_version : (9ull | (1ull << 56)), 0, nData, 0, csz / 8, 0, bsz, 0, nsz,
                     (uint64_t)((uintptr_t)__start___llvm_prf_cnts - (uintptr_t)db), (uint64_t)((uintptr_t)__start___llvm_prf_bits - (uintptr_t)db), (uint64_t)(uintptr_t)__start___llvm_prf_names, 1 };
  h[4] = (8 - dsz % 8) % 8; h[6] = (8 - csz % 8) % 8; h[8] = (8 - bsz % 8) % 8;
  fwrite(h, 8, 14, f); fwrite(db, 1, rawd, f); if (dsz > rawd) fwrite(zeros, 1, dsz - rawd, f); pad(f, dsz);
  fwrite(__start___llvm_prf_cnts, 1, csz, f); pad(f, csz); fwrite(__start___llvm_prf_bits, 1, bsz, f); pad(f, bsz); fwrite(__start___llvm_prf_names, 1, nsz, f); pad(f, nsz);
  for (Data *d = db; (char *)d + 44 <= (char *)de; d++) {                  /* value profile data */
    uint32_t kinds = 0, total = 8; for (int k = 0; k < 2; k++) if (d->NumValueSites[k]) kinds++;
    if (!kinds) continue;
    VNode **heads = (VNode **)d->Values; uint32_t base = 0; uint32_t hdr[2];
    for (int k = 0; k < 2; k++) {                    /* sizes first */
      uint32_t ns = d->NumValueSites[k]; if (!ns) continue; uint32_t nv = 0;
      for (uint32_t s = 0; s < ns; s++) { uint32_t c = 0; for (VNode *n = heads[base + s]; n && c < 255; n = n->Next) c++; nv += c; }
      total += ((8 + ns + 7) & ~7u) + 16 * nv; base += ns;
    }
    hdr[0] = total; hdr[1] = kinds; fwrite(hdr, 4, 2, f); base = 0;
    for (int k = 0; k < 2; k++) {
      uint32_t ns = d->NumValueSites[k]; if (!ns) continue; uint32_t rh[2] = { (uint32_t)k, ns }; uint8_t *cnt = (uint8_t *)calloc(1, ns + 8);
      for (uint32_t s = 0; s < ns; s++) { uint32_t c = 0; for (VNode *n = heads[base + s]; n && c < 255; n = n->Next) c++; cnt[s] = (uint8_t)c; }
      fwrite(rh, 4, 2, f); fwrite(cnt, 1, ((8 + ns + 7) & ~7u) - 8, f); free(cnt);
      for (uint32_t s = 0; s < ns; s++) { uint32_t c = 0; for (VNode *n = heads[base + s]; n && c < 255; n = n->Next, c++) { uint64_t vd[2] = { n->Value, n->Count }; fwrite(vd, 8, 2, f); } }
      base += ns;
    }
  }
  fclose(f); return 0;
}
