/* NPP2: QEMU TCG plugin that lists the memory words the game thread and the frame-send worker touch in the same window
 * (the native counterpart of the recompiled build's race check, split/NOTES.md "Method"; there TSAN-instrumented shards, here qemu's
 * memory callbacks, so it sees every access of the armhf binary: game, SDK layer, vgx, libc).
 *
 * The game marks the windows with the (nonexistent) syscall 0x7f00 (np_framesend.cpp, MSC_FS_MARKS / MSC_QEMU_MARK):
 *   0x30000001 fork (game thread): the window opens          0x30000002 join done (game thread): the window closes, conflicts are listed
 *   0x30000003 job begin (worker)                            0x30000004 job end (worker)
 * and the harness' frame marks (value < 0x20000000) give the frame number. Between fork and join the game thread's accesses go into
 * table M, the worker's accesses between job begin and job end into table W (4-byte words, the pc of the first read and of the first
 * write of each word). At the join the words in both tables are classified:
 *   A  game thread WRITES, worker READS    (the picture may use the next step's value: what the send must be protected from)
 *   B  both WRITE                          (a shared counter or state word)
 *   C  game thread READS, worker WRITES    (the game thread looks at something before the join that the job changes)
 * and aggregated by (kind, pc of the game thread's access, pc of the worker's access). The summary goes to the plugin output at exit:
 *   [race] kind=A words=<n> windows=<n> game_pc=<hex> worker_pc=<hex> addr=<hex> frames=<first>-<last>
 * Symbolise with tools/perf/racereport.py (nm of the executable + the run's mappings).
 *
 * Options: from=<frame>,to=<frame> (only windows whose frame is in the range), all=1 (also list words only read by both: no).
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MARK_SYSCALL 0x7f00
#define MAXCPU 64
#ifndef TBITS
#define TBITS 21     /* the pipeline's window is a whole build frame: build with -DTBITS=23 (tools/pipe/run_race.sh uses plug/libracecheck.so built so) */
#endif
#define TCAP (1u << TBITS)

typedef struct {
    uint32_t word;      /* address >> 2 */
    uint16_t gen;
    uint8_t flags;      /* 1 read, 2 write */
    uint8_t pad;
    uint32_t pcr, pcw;  /* first read / first write */
} Ent;

typedef struct {
    Ent *e;
    uint32_t *list;     /* slots inserted in this generation */
    uint32_t n;
    uint16_t gen;
    gboolean overflow;
} Table;

static Table tm;                 /* the game thread's accesses in the window */
#define MAXOTHER 8
static Table to[MAXOTHER];       /* the accesses of every other vCPU in the window (the worker, the render thread, helpers), one table each */
static gboolean to_init[MAXOTHER];
static volatile int window_on;
static int main_vcpu = -1, worker_vcpu = -1;
static int64_t last_frame = -1, from_frame = -1, to_frame = -1;
static gboolean window_open;
static uint64_t windows, windows_recorded, overflows;
static qemu_plugin_id_t g_id;
static gboolean mem_on;          /* memory callbacks are being inserted into newly translated blocks */
static gboolean reset_pending;
static gboolean range_done;

typedef struct { uint32_t kind, pcm, pcw, vcpu; } Key;
typedef struct { uint64_t words, windows; int64_t last_seen_frame, first, last; uint32_t addr; } Agg;
static GHashTable *agg;
static uint64_t conflict_windows;

static guint key_hash(gconstpointer p) { const Key *k = p; return k->kind * 2654435761u ^ k->pcm * 40503u ^ k->pcw * 9973u; }
static gboolean key_eq(gconstpointer a, gconstpointer b) { return !memcmp(a, b, sizeof(Key)); }

static void table_init(Table *t)
{
    t->e = g_malloc0(sizeof(Ent) * TCAP);
    t->list = g_malloc(sizeof(uint32_t) * TCAP);
    t->n = 0;
    t->gen = 1;
    t->overflow = FALSE;
}

static void table_reset(Table *t)
{
    t->n = 0;
    t->overflow = FALSE;
    if (++t->gen == 0) {            /* wrapped: clear */
        memset(t->e, 0, sizeof(Ent) * TCAP);
        t->gen = 1;
    }
}

static inline Ent *table_slot(Table *t, uint32_t word, gboolean insert)
{
    uint32_t h = (word * 2654435761u) >> (32 - TBITS);
    for (;;) {
        Ent *e = &t->e[h];
        if (e->gen != t->gen) {
            if (!insert) return NULL;
            if (t->n >= TCAP / 4 * 3) { t->overflow = TRUE; return NULL; }
            e->gen = t->gen; e->word = word; e->flags = 0; e->pcr = 0; e->pcw = 0;
            t->list[t->n++] = h;
            return e;
        }
        if (e->word == word) return e;
        h = (h + 1) & (TCAP - 1);
    }
}

static void mem_cb(unsigned int vcpu, qemu_plugin_meminfo_t info, uint64_t vaddr, void *udata)
{
    if (!window_on || vcpu >= MAXCPU) return;
    Table *t;
    if ((int)vcpu == main_vcpu) t = &tm;
    else if (vcpu < MAXOTHER && to_init[vcpu]) t = &to[vcpu];
    else return;
    uint32_t pc = (uint32_t)(uintptr_t)udata;
    unsigned size = 1u << qemu_plugin_mem_size_shift(info);
    gboolean st = qemu_plugin_mem_is_store(info);
    uint32_t w0 = (uint32_t)(vaddr >> 2), w1 = (uint32_t)((vaddr + size - 1) >> 2);
    for (uint32_t w = w0; w <= w1; w++) {
        Ent *e = table_slot(t, w, TRUE);
        if (!e) return;
        if (st) { if (!(e->flags & 2)) { e->flags |= 2; e->pcw = pc; } }
        else    { if (!(e->flags & 1)) { e->flags |= 1; e->pcr = pc; } }
    }
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    if (!mem_on) return;
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_mem_cb(in, mem_cb, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW,
                                         (void *)(uintptr_t)qemu_plugin_insn_vaddr(in));
    }
}

static void note(uint32_t kind, uint32_t pcm, uint32_t pcw, uint32_t word, uint32_t vcpu)
{
    Key k = { kind, pcm, pcw, vcpu };
    Agg *a = g_hash_table_lookup(agg, &k);
    if (!a) {
        Key *nk = g_memdup2(&k, sizeof k);
        a = g_new0(Agg, 1);
        a->first = last_frame;
        a->addr = word << 2;
        g_hash_table_insert(agg, nk, a);
    }
    a->words++;
    a->last = last_frame;
    if (a->last_seen_frame != last_frame) { a->last_seen_frame = last_frame; a->windows++; }
}

static void close_window(void)
{
    uint64_t found = 0;
    if (tm.overflow) overflows++;
    for (int v = 0; v < MAXOTHER; v++) {
        if (!to_init[v]) continue;
        Table *o = &to[v];
        if (o->overflow) overflows++;
        for (uint32_t i = 0; i < o->n; i++) {
            Ent *w = &o->e[o->list[i]];
            Ent *m = table_slot(&tm, w->word, FALSE);
            if (!m) continue;
            if ((m->flags & 2) && (w->flags & 1)) { note('A', m->pcw, w->pcr, w->word, v); found++; }
            if ((m->flags & 2) && (w->flags & 2)) { note('B', m->pcw, w->pcw, w->word, v); found++; }
            if ((m->flags & 1) && (w->flags & 2)) { note('C', m->pcr, w->pcw, w->word, v); found++; }
        }
        table_reset(o);
    }
    if (found) conflict_windows++;
    table_reset(&tm);
}

static void plugin_exit(qemu_plugin_id_t id, void *p);
static void on_syscall(qemu_plugin_id_t id, unsigned int vcpu, int64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8);

/* The memory callback costs every memory access of every thread, so it is only compiled into the blocks while the frame range is
 * running: qemu_plugin_reset drops every callback and every translated block, the reset callback puts back what is wanted. */
static void vcpu_init(qemu_plugin_id_t id, unsigned int vcpu)
{
    if (vcpu < MAXOTHER && !to_init[vcpu]) { table_init(&to[vcpu]); to_init[vcpu] = TRUE; }
}

static void reinstall(gboolean with_mem)
{
    mem_on = with_mem;
    if (with_mem) qemu_plugin_register_vcpu_tb_trans_cb(g_id, tb_trans);
    qemu_plugin_register_vcpu_syscall_cb(g_id, on_syscall);
    qemu_plugin_register_atexit_cb(g_id, plugin_exit, NULL);
    qemu_plugin_register_vcpu_init_cb(g_id, vcpu_init);
    reset_pending = FALSE;
}
static void reinstall_on(qemu_plugin_id_t id) { reinstall(TRUE); }
static void reinstall_off(qemu_plugin_id_t id) { reinstall(FALSE); }

static void on_syscall(qemu_plugin_id_t id, unsigned int vcpu, int64_t num, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
{
    if (num != MARK_SYSCALL) return;
    if (a1 < 0x20000000) {
        last_frame = (int64_t)a1;
        if (main_vcpu < 0) main_vcpu = (int)vcpu;
        if (!reset_pending && !range_done) {
            if (!mem_on && (from_frame < 0 || last_frame >= from_frame) && (to_frame < 0 || last_frame <= to_frame)) {
                reset_pending = TRUE;
                qemu_plugin_reset(g_id, reinstall_on);
            } else if (mem_on && to_frame >= 0 && last_frame > to_frame) {
                range_done = TRUE; reset_pending = TRUE;
                qemu_plugin_reset(g_id, reinstall_off);
            }
        }
        return;
    }
    switch (a1) {
    case 0x30000001:                               /* fork */
        main_vcpu = (int)vcpu;
        windows++;
        if (mem_on && !reset_pending && (from_frame < 0 || last_frame >= from_frame) && (to_frame < 0 || last_frame <= to_frame)) {
            window_open = TRUE; windows_recorded++;
            table_reset(&tm);
            for (int v = 0; v < MAXOTHER; v++) if (to_init[v]) table_reset(&to[v]);
            window_on = 1;
        } else window_open = FALSE;
        break;
    case 0x30000002:                               /* join done */
        if (window_open) {
            window_on = 0;
            close_window();
            window_open = FALSE;
        }
        break;
    case 0x30000003:                               /* job begin */
        if ((int)vcpu != main_vcpu) worker_vcpu = (int)vcpu;
        break;
    case 0x30000004:
        break;
    default:
        break;
    }
}

static gint cmp_agg(gconstpointer a, gconstpointer b, gpointer data)
{
    GHashTable *t = data;
    const Agg *x = g_hash_table_lookup(t, a), *y = g_hash_table_lookup(t, b);
    return x->words < y->words ? 1 : x->words > y->words ? -1 : 0;
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    GList *keys = g_hash_table_get_keys(agg);
    keys = g_list_sort_with_data(keys, cmp_agg, agg);
    qemu_plugin_outs(g_strdup_printf("[racesum] windows=%" PRIu64 " recorded=%" PRIu64 " with_conflicts=%" PRIu64 " table_overflows=%" PRIu64 " main_vcpu=%d worker_vcpu=%d groups=%u\n",
                                     windows, windows_recorded, conflict_windows, overflows, main_vcpu, worker_vcpu, g_hash_table_size(agg)));
    for (GList *l = keys; l; l = l->next) {
        const Key *k = l->data;
        const Agg *a = g_hash_table_lookup(agg, k);
        qemu_plugin_outs(g_strdup_printf("[race] kind=%c vcpu=%u%s words=%" PRIu64 " windows=%" PRIu64 " game_pc=%x worker_pc=%x addr=%x frames=%" PRId64 "-%" PRId64 "\n",
                                         k->kind, k->vcpu, (int)k->vcpu == worker_vcpu ? "(worker)" : "", a->words, a->windows, k->pcm, k->pcw, a->addr, a->first, a->last));
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "from=")) from_frame = g_ascii_strtoll(argv[i] + 5, NULL, 10);
        else if (g_str_has_prefix(argv[i], "to=")) to_frame = g_ascii_strtoll(argv[i] + 3, NULL, 10);
    }
    table_init(&tm);
    agg = g_hash_table_new_full(key_hash, key_eq, g_free, g_free);
    g_id = id;
    mem_on = (from_frame < 0);       /* no range: instrument from the start */
    if (mem_on) qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_vcpu_syscall_cb(id, on_syscall);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    return 0;
}
