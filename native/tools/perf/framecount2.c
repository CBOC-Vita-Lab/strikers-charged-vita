/* NPP copy of tools/qemu-plugin/framecount.c. Difference: the "game thread" column of the profile is the vCPU that issues the
 * FIRST frame mark (the native port runs the game on a thread of its own, not on vCPU 0); `game=<n>` forces a vCPU.
 *
 * QEMU TCG plugin for the port's performance work.
 *
 * 1. Frame marks: the guest calls the (nonexistent) syscall MARK_SYSCALL with a
 *    frame number; each call prints every guest thread's cumulative instruction
 *    count ("[insn] mark frame=N t0=... t1=...").
 * 2. Profile: with  profile=<file>,from=<frame>[,to=<frame>]  the plugin
 *    records how many instructions each translated block executed between the
 *    two marks, per thread class (thread 0 = game thread, others summed), and
 *    writes "pc insns_thread0 insns_other" lines to <file> at exit.
 */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
#define MARK_SYSCALL 0x7f00
#define AI_SYSCALL 0x7f01   /* PIPE: single-player AI script time budget accounting (platform/compat/np_aibudget.cpp, MSC_AI_MARKS=1) */

static struct qemu_plugin_scoreboard *score;
static qemu_plugin_u64 insns;

#define MAXW 8
typedef struct {
    uint64_t pc;
    uint32_t n_insns;
    struct qemu_plugin_scoreboard *execs;
    uint64_t base0, baseOther;   /* exec counts at the "from" mark */
    uint64_t end0, endOther;     /* exec counts at the "to" mark */
    uint64_t wbase0, wbaseOther; /* windows=: counts when the open window started */
    uint64_t wacc0[MAXW], waccOther[MAXW];   /* windows=: instructions executed inside window w (executions x block length are applied at output) */
} Block;

static GHashTable *blocks;
static GMutex lock;
static const char *profile_path;
static int64_t prof_from = -1, prof_to = -1;
static gboolean have_base, have_end;
static uint64_t trace_pc;               /* trace=<hex>: charge the instructions executed between consecutive entries at this pc to the caller (lr) */
static GHashTable *trace_tab;           /* lr -> TraceEnt */
typedef struct { uint64_t calls, insns, max; } TraceEnt;
typedef struct { uint64_t lr, insns; int have; struct qemu_plugin_register *r_lr; } TraceLast;
static TraceLast tlast[64];
/* windows=a-b+c-d: several profile windows in ONE run, written to <profile>.0, <profile>.1, ... (frame numbers of the marks) */
static int nwin;
static int64_t win_from[MAXW], win_to[MAXW];
static int win_state[MAXW];   /* 0 waiting, 1 open, 2 done */
static int game_vcpu = -1;        /* -1 until the first mark; "game=N" forces it */
static gboolean game_forced;

static void window_snapshot(int w, gboolean closing)
{
    GHashTableIter it;
    gpointer k, v;
    g_mutex_lock(&lock);
    g_hash_table_iter_init(&it, blocks);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Block *b = v;
        qemu_plugin_u64 e = qemu_plugin_scoreboard_u64(b->execs);
        uint64_t t0 = qemu_plugin_u64_get(e, game_vcpu < 0 ? 0 : game_vcpu);
        uint64_t all = qemu_plugin_u64_sum(e);
        if (closing) {
            b->wacc0[w] += t0 - b->wbase0;
            b->waccOther[w] += (all - t0) - b->wbaseOther;
        } else {
            b->wbase0 = t0;
            b->wbaseOther = all - t0;
        }
    }
    g_mutex_unlock(&lock);
}

static void trace_charge(uint64_t lr, uint64_t d)
{
    g_mutex_lock(&lock);
    TraceEnt *e = g_hash_table_lookup(trace_tab, (gconstpointer)lr);
    if (!e) {
        e = g_new0(TraceEnt, 1);
        g_hash_table_insert(trace_tab, (gpointer)lr, e);
    }
    e->calls++;
    e->insns += d;
    if (d > e->max) {
        e->max = d;
    }
    g_mutex_unlock(&lock);
}

static void trace_cb(unsigned int vcpu, void *udata)
{
    if (vcpu >= 64 || !tlast[vcpu].r_lr) {
        return;
    }
    g_autoptr(GByteArray) buf = g_byte_array_new();
    if (qemu_plugin_read_register(tlast[vcpu].r_lr, buf) < 4) {
        return;
    }
    uint64_t lr = *(uint32_t *)buf->data;
    uint64_t cur = qemu_plugin_u64_get(insns, vcpu);
    if (tlast[vcpu].have) {
        trace_charge(tlast[vcpu].lr, cur - tlast[vcpu].insns);
    }
    tlast[vcpu].lr = lr;
    tlast[vcpu].insns = cur;
    tlast[vcpu].have = 1;
}

static void vcpu_init(qemu_plugin_id_t id, unsigned int vcpu)
{
    if (!trace_pc || vcpu >= 64) {
        return;
    }
    g_autoptr(GArray) regs = qemu_plugin_get_registers();
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *r = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (!g_strcmp0(r->name, "lr") || !g_strcmp0(r->name, "r14")) {
            tlast[vcpu].r_lr = r->handle;
        }
    }
}

static void snapshot(gboolean end)
{
    GHashTableIter it;
    gpointer k, v;
    g_mutex_lock(&lock);
    g_hash_table_iter_init(&it, blocks);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Block *b = v;
        qemu_plugin_u64 e = qemu_plugin_scoreboard_u64(b->execs);
        uint64_t t0 = qemu_plugin_u64_get(e, game_vcpu < 0 ? 0 : game_vcpu);
        uint64_t all = qemu_plugin_u64_sum(e);
        if (end) { b->end0 = t0; b->endOther = all - t0; }
        else { b->base0 = t0; b->baseOther = all - t0; }
    }
    g_mutex_unlock(&lock);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64, insns, n);
    if (trace_pc) {
        for (size_t i = 0; i < n; i++) {
            struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
            if ((qemu_plugin_insn_vaddr(in) & ~1ull) == trace_pc) {
                qemu_plugin_register_vcpu_insn_exec_cb(in, trace_cb, QEMU_PLUGIN_CB_R_REGS, NULL);
            }
        }
    }
    if (!profile_path) {
        return;
    }
    uint64_t pc = qemu_plugin_tb_vaddr(tb);
    uint64_t key = pc ^ ((uint64_t)n << 48);
    g_mutex_lock(&lock);
    Block *b = g_hash_table_lookup(blocks, (gconstpointer)key);
    if (!b) {
        b = g_new0(Block, 1);
        b->pc = pc;
        b->n_insns = n;
        b->execs = qemu_plugin_scoreboard_new(sizeof(uint64_t));
        g_hash_table_insert(blocks, (gpointer)key, b);
    }
    g_mutex_unlock(&lock);
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, qemu_plugin_scoreboard_u64(b->execs), 1);
}

static void dump(const char *tag, int64_t frame, unsigned int vcpu)
{
    g_autoptr(GString) s = g_string_new(NULL);
    int n = qemu_plugin_num_vcpus();
    g_string_append_printf(s, "[insn] %s frame=%" PRId64 " caller=%u", tag, frame, vcpu);
    for (int i = 0; i < n; i++) {
        g_string_append_printf(s, " t%d=%" PRIu64, i, qemu_plugin_u64_get(insns, i));
    }
    g_string_append_c(s, '\n');
    qemu_plugin_outs(s->str);
}


/* ---- AI script accounting (syscall 0x7f01, a1 = 1 bracket begin, 2 bracket end, 3 budget check, 4 update end) ----------------------------
 * Per update the instructions spent in the script brackets, as the retail clock would add them (every bracket counts, nested ones twice:
 * "retail") and without double counting ("outer"), plus, at every budget check, what the retail rule would skip if one millisecond were K
 * thousand instructions: skip probability 0.8 * min(acc / K, 1) with acc the retail sum so far in this update. Table at exit. */
#define AI_MAXV 64
#define AI_DEPTH 32
#define AI_NK 9
static const double ai_K[AI_NK] = { 100, 150, 230, 300, 400, 500, 700, 1000, 2000 };   /* thousand instructions per ms */
#define AI_BUCKETS 400    /* 10 thousand instructions each: up to 4 M per update */
static struct {
    uint64_t begin[AI_DEPTH];
    int depth;
    double acc, outer;
    uint64_t checks_upd;
} ai_v[AI_MAXV];
static struct {
    uint64_t updates, checks, brackets;
    double sumP[AI_NK], sumRetail, sumOuter, maxRetail, maxOuter;
    uint64_t sat[AI_NK];
    uint64_t hist[AI_BUCKETS + 1];
    int perupd;
    uint64_t seq;
} ai_s;

static void ai_syscall(unsigned int vcpu, uint64_t op)
{
    if (vcpu >= AI_MAXV) {
        return;
    }
    uint64_t cur = qemu_plugin_u64_get(insns, vcpu);
    switch (op) {
    case 1:
        if (ai_v[vcpu].depth < AI_DEPTH) {
            ai_v[vcpu].begin[ai_v[vcpu].depth] = cur;
        }
        ai_v[vcpu].depth++;
        break;
    case 2:
        if (ai_v[vcpu].depth > 0) {
            ai_v[vcpu].depth--;
            if (ai_v[vcpu].depth < AI_DEPTH) {
                double d = (double)(cur - ai_v[vcpu].begin[ai_v[vcpu].depth]);
                ai_v[vcpu].acc += d;
                if (ai_v[vcpu].depth == 0) {
                    ai_v[vcpu].outer += d;
                }
                ai_s.brackets++;
            }
        }
        break;
    case 3: {
        double acc = ai_v[vcpu].acc;
        ai_s.checks++;
        ai_v[vcpu].checks_upd++;
        for (int i = 0; i < AI_NK; i++) {
            double t = acc / (ai_K[i] * 1000.0);
            ai_s.sumP[i] += 0.8 * (t < 1.0 ? t : 1.0);
        }
        break;
    }
    case 4: {
        double acc = ai_v[vcpu].acc;
        ai_s.updates++;
        ai_s.sumRetail += acc;
        ai_s.sumOuter += ai_v[vcpu].outer;
        if (acc > ai_s.maxRetail) ai_s.maxRetail = acc;
        if (ai_v[vcpu].outer > ai_s.maxOuter) ai_s.maxOuter = ai_v[vcpu].outer;
        for (int i = 0; i < AI_NK; i++) {
            if (acc / (ai_K[i] * 1000.0) >= 1.0) ai_s.sat[i]++;
        }
        size_t b = (size_t)(acc / 10000.0);
        ai_s.hist[b > AI_BUCKETS ? AI_BUCKETS : b]++;
        if (ai_s.perupd) {
            qemu_plugin_outs(g_strdup_printf("[ai] upd=%" PRIu64 " retail=%.0f outer=%.0f checks=%" PRIu64 "\n", ai_s.seq, acc, ai_v[vcpu].outer,
                                             ai_v[vcpu].checks_upd));
        }
        ai_s.seq++;
        ai_v[vcpu].acc = ai_v[vcpu].outer = 0;
        ai_v[vcpu].checks_upd = 0;
        ai_v[vcpu].depth = 0;
        break;
    }
    default:
        break;
    }
}

static void ai_report(void)
{
    if (!ai_s.updates) {
        return;
    }
    g_autoptr(GString) s = g_string_new(NULL);
    g_string_append_printf(s, "[aiscript] updates=%" PRIu64 " brackets=%" PRIu64 " (%.1f per update) checks=%" PRIu64 " (%.1f per update)\n", ai_s.updates,
                           ai_s.brackets, (double)ai_s.brackets / ai_s.updates, ai_s.checks, (double)ai_s.checks / ai_s.updates);
    g_string_append_printf(s, "[aiscript] instructions in script brackets per update: retail sum mean %.0f max %.0f; outermost only mean %.0f max %.0f\n",
                           ai_s.sumRetail / ai_s.updates, ai_s.maxRetail, ai_s.sumOuter / ai_s.updates, ai_s.maxOuter);
    uint64_t acc = 0;
    double q[3] = { 0.5, 0.9, 0.99 }, qv[3] = { 0, 0, 0 };
    int qi = 0;
    for (int b = 0; b <= AI_BUCKETS && qi < 3; b++) {
        acc += ai_s.hist[b];
        while (qi < 3 && (double)acc >= q[qi] * (double)ai_s.updates) {
            qv[qi++] = (b + 1) * 10000.0;
        }
    }
    g_string_append_printf(s, "[aiscript] retail sum per update: p50 %.0f p90 %.0f p99 %.0f (bucket upper bound, 10000 instructions)\n", qv[0], qv[1], qv[2]);
    for (int i = 0; i < AI_NK; i++) {
        g_string_append_printf(s, "[aiscript] 1 ms = %4.0f thousand instructions: retail rule skips %5.2f %% of the scripts; %5.1f %% of the updates reach the budget\n",
                               ai_K[i], ai_s.checks ? 100.0 * ai_s.sumP[i] / ai_s.checks : 0.0, 100.0 * ai_s.sat[i] / ai_s.updates);
    }
    qemu_plugin_outs(s->str);
}

static void on_syscall(qemu_plugin_id_t id, unsigned int vcpu, int64_t num,
                       uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
{
    if (num == AI_SYSCALL) {
        ai_syscall(vcpu, a1);
        return;
    }
    if (num != MARK_SYSCALL) {
        return;
    }
    if (game_vcpu < 0 && a1 < 0x20000000) {
        game_vcpu = (int)vcpu;
    }
    dump("mark", (int64_t)a1, vcpu);
    if (a1 < 0x20000000) {
        for (int w = 0; w < nwin; w++) {
            if (win_state[w] == 0 && (int64_t)a1 >= win_from[w]) {
                /* a window opens at the first mark at or after its start; windows must not overlap (one base per block) */
                window_snapshot(w, FALSE);
                win_state[w] = 1;
            } else if (win_state[w] == 1 && (int64_t)a1 >= win_to[w]) {
                window_snapshot(w, TRUE);
                win_state[w] = 2;
            }
        }
    }
    if (profile_path && !have_base && prof_from >= 0 && (int64_t)a1 >= prof_from) {
        snapshot(FALSE);
        have_base = TRUE;
    }
    if (profile_path && have_base && !have_end && prof_to >= 0 && (int64_t)a1 >= prof_to) {
        snapshot(TRUE);
        have_end = TRUE;
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    dump("exit", -1, 0);
    ai_report();
    if (trace_pc && trace_tab) {
        GList *keys = g_hash_table_get_keys(trace_tab), *l;
        GPtrArray *arr = g_ptr_array_new();
        for (l = keys; l; l = l->next) {
            g_ptr_array_add(arr, l->data);
        }
        for (guint i = 0; i < arr->len; i++) {            /* selection sort of the top 40 by insns */
            for (guint j = i + 1; j < arr->len; j++) {
                TraceEnt *a = g_hash_table_lookup(trace_tab, arr->pdata[i]);
                TraceEnt *b = g_hash_table_lookup(trace_tab, arr->pdata[j]);
                if (b->insns > a->insns) {
                    gpointer t = arr->pdata[i]; arr->pdata[i] = arr->pdata[j]; arr->pdata[j] = t;
                }
            }
            if (i >= 40) {
                break;
            }
        }
        for (guint i = 0; i < arr->len && i < 40; i++) {
            TraceEnt *e = g_hash_table_lookup(trace_tab, arr->pdata[i]);
            qemu_plugin_outs(g_strdup_printf("[trace] lr=%" PRIx64 " calls=%" PRIu64 " insns=%" PRIu64 " max=%" PRIu64 "\n",
                                             (uint64_t)(uintptr_t)arr->pdata[i], e->calls, e->insns, e->max));
        }
    }
    if (!profile_path) {
        return;
    }
    if (!have_end) {
        snapshot(TRUE);
    }
    for (int w = 0; w < nwin; w++) {
        if (win_state[w] == 1) {
            window_snapshot(w, TRUE);
        }
        g_autofree char *name = g_strdup_printf("%s.%d", profile_path, w);
        FILE *wf = fopen(name, "w");
        if (wf) {
            GHashTableIter wit;
            gpointer wk, wv;
            g_hash_table_iter_init(&wit, blocks);
            while (g_hash_table_iter_next(&wit, &wk, &wv)) {
                Block *b = wv;
                uint64_t d0 = b->wacc0[w] * b->n_insns, d1 = b->waccOther[w] * b->n_insns;
                if (d0 || d1) {
                    fprintf(wf, "%" PRIx64 " %" PRIu64 " %" PRIu64 "\n", b->pc, d0, d1);
                }
            }
            fclose(wf);
        }
    }
    FILE *f = fopen(profile_path, "w");
    if (!f) {
        return;
    }
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, blocks);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Block *b = v;
        uint64_t d0 = (b->end0 - b->base0) * b->n_insns;
        uint64_t d1 = (b->endOther - b->baseOther) * b->n_insns;
        if (d0 || d1) {
            fprintf(f, "%" PRIx64 " %" PRIu64 " %" PRIu64 "\n", b->pc, d0, d1);
        }
    }
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "profile=")) {
            profile_path = g_strdup(argv[i] + 8);
        } else if (g_str_has_prefix(argv[i], "windows=")) {
            gchar **parts = g_strsplit(argv[i] + 8, "+", MAXW);
            for (int k = 0; parts[k] && nwin < MAXW; k++) {
                int64_t a = 0, b = 0;
                if (sscanf(parts[k], "%" SCNd64 "-%" SCNd64, &a, &b) == 2 && b > a) {
                    win_from[nwin] = a; win_to[nwin] = b; nwin++;
                }
            }
            g_strfreev(parts);
        } else if (g_str_has_prefix(argv[i], "trace=")) {
            trace_pc = g_ascii_strtoull(argv[i] + 6, NULL, 16) & ~1ull;
        } else if (g_str_has_prefix(argv[i], "aiupd")) {
            ai_s.perupd = 1;
        } else if (g_str_has_prefix(argv[i], "game=")) {
            game_vcpu = (int)g_ascii_strtoll(argv[i] + 5, NULL, 10);
            game_forced = TRUE;
        } else if (g_str_has_prefix(argv[i], "from=start")) {
            /* profile from the first instruction (block counters start at 0): "from=start,to=N" covers the whole start-up */
            have_base = TRUE;
        } else if (g_str_has_prefix(argv[i], "from=")) {
            prof_from = g_ascii_strtoll(argv[i] + 5, NULL, 10);
        } else if (g_str_has_prefix(argv[i], "to=")) {
            prof_to = g_ascii_strtoll(argv[i] + 3, NULL, 10);
        }
    }
    blocks = g_hash_table_new(NULL, g_direct_equal);
    trace_tab = g_hash_table_new(NULL, g_direct_equal);
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    score = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    insns = qemu_plugin_scoreboard_u64(score);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_vcpu_syscall_cb(id, on_syscall);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
