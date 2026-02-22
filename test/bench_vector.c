/*
 * bench_vector.c
 * Performance benchmark: brute-force vs IVF search on 1 M vectors.
 *
 * Compiled with -DSQLITE_CORE so sqlite3_vector_init links statically.
 * Usage: make benchmark
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sqlite3.h"
#include "sqlite-vector.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static double now_ms(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / freq.QuadPart * 1000.0;
}
static double get_rss_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    return 0.0;
}
#elif defined(__APPLE__)
#include <sys/time.h>
#include <mach/mach.h>
static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
static double get_rss_mb(void) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return (double)info.resident_size / (1024.0 * 1024.0);
    return 0.0;
}
#else
#include <sys/time.h>
static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}
static double get_rss_mb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0.0;
    char line[256];
    double rss = 0.0;
    while (fgets(line, sizeof(line), f)) {
        long val;
        if (sscanf(line, "VmRSS: %ld kB", &val) == 1) { rss = val / 1024.0; break; }
    }
    fclose(f);
    return rss;
}
#endif

/* ---------- Test infrastructure ---------- */

static int failures = 0;
static int passes   = 0;

#define ASSERT(cond, msg) do {                        \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("PASS: %s\n", msg); passes++;    } \
} while (0)

/* ---------- Benchmark parameters ---------- */

#define BENCH_N         1000000     /* number of vectors                */
#define BENCH_DIM       768         /* vector dimension                 */
#define BENCH_K         10          /* top-k for search                 */
#define BENCH_NLIST     100         /* IVF number of clusters           */
#define BENCH_NPROBE    20          /* IVF clusters to search           */

/* ---------- Portable xorshift32 PRNG ---------- */

static uint32_t bench_xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static float rand_float(uint32_t *state) {
    return (float)(bench_xorshift32(state) & 0xFFFF) / 65536.0f;
}

/* ---------- Helpers ---------- */

static int exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("  SQL error (%d): %s\n  Statement: %.120s\n", rc, err ? err : "unknown", sql);
        sqlite3_free(err);
    }
    return rc;
}

/* Run a top-k search and collect rowids.  Returns the number of rows. */
static int run_topk_search(sqlite3 *db, const char *vtab, const char *table,
                           const float *query, int dim, int k,
                           int64_t *out_ids, double *elapsed_ms) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT id, distance FROM %s('%s', 'v', ?, %d);", vtab, table, k);

    sqlite3_stmt *vm = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &vm, NULL);
    if (rc != SQLITE_OK) { *elapsed_ms = -1; return 0; }

    rc = sqlite3_bind_blob(vm, 1, query, dim * (int)sizeof(float), SQLITE_STATIC);
    if (rc != SQLITE_OK) { sqlite3_finalize(vm); *elapsed_ms = -1; return 0; }

    int count = 0;
    double t0 = now_ms();
    while (sqlite3_step(vm) == SQLITE_ROW && count < k) {
        out_ids[count++] = sqlite3_column_int64(vm, 0);
    }
    *elapsed_ms = now_ms() - t0;
    sqlite3_finalize(vm);
    return count;
}

/* Compute recall: |intersection(a,b)| / |a|  */
static double compute_recall(const int64_t *truth, int ntruth,
                             const int64_t *pred,  int npred) {
    if (ntruth == 0) return 0.0;
    int hits = 0;
    for (int i = 0; i < npred; i++) {
        for (int j = 0; j < ntruth; j++) {
            if (pred[i] == truth[j]) { hits++; break; }
        }
    }
    return (double)hits / ntruth;
}

/* ---------- Main benchmark ---------- */

int main(void) {
    printf("=== IVF Benchmark ===\n");
    printf("  Vectors: %d   Dim: %d   K: %d   nlist: %d   nprobe: %d\n\n",
           BENCH_N, BENCH_DIM, BENCH_K, BENCH_NLIST, BENCH_NPROBE);

    /* ---- open DB ---- */
    sqlite3 *db;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) { printf("FAIL: cannot open database\n"); return 1; }

    char *errmsg = NULL;
    rc = sqlite3_vector_init(db, &errmsg, NULL);
    if (rc != SQLITE_OK) {
        printf("FAIL: sqlite3_vector_init: %s\n", errmsg ? errmsg : "");
        sqlite3_close(db);
        return 1;
    }

    /* ---- create table ---- */
    exec_sql(db, "CREATE TABLE bench (id INTEGER PRIMARY KEY, v BLOB);");

    /* ---- insert 1 M vectors inside a transaction ---- */
    printf("Inserting %d vectors ...\n", BENCH_N);
    double t0 = now_ms();
    exec_sql(db, "BEGIN;");

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO bench (id, v) VALUES (?, ?);", -1, &ins, NULL);

    float *vec = (float *)malloc(BENCH_DIM * sizeof(float));
    uint32_t rng = 12345u;

    for (int i = 0; i < BENCH_N; i++) {
        for (int d = 0; d < BENCH_DIM; d++) vec[d] = rand_float(&rng);
        sqlite3_bind_int(ins, 1, i + 1);
        sqlite3_bind_blob(ins, 2, vec, BENCH_DIM * (int)sizeof(float), SQLITE_STATIC);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    exec_sql(db, "COMMIT;");
    double t_insert = now_ms() - t0;
    printf("  Insert:       %.1f ms\n", t_insert);

    /* ---- vector_init ---- */
    {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT vector_init('bench', 'v', 'type=f32,dimension=%d,distance=L2');",
                 BENCH_DIM);
        exec_sql(db, sql);
    }

    /* ---- generate query vector ---- */
    float *query = (float *)malloc(BENCH_DIM * sizeof(float));
    uint32_t qrng = 99999u;
    for (int d = 0; d < BENCH_DIM; d++) query[d] = rand_float(&qrng);

    /* ---- brute-force search ---- */
    int64_t bf_ids[BENCH_K];
    double t_brute;
    double mem_before_bf = get_rss_mb();
    int bf_count = run_topk_search(db, "vector_full_scan", "bench",
                                   query, BENCH_DIM, BENCH_K,
                                   bf_ids, &t_brute);
    double mem_after_bf = get_rss_mb();
    printf("  Brute force:  %.1f ms  (%d results)  RSS: %.1f MB\n", t_brute, bf_count, mem_after_bf);

    /* ---- IVF build ---- */
    {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT vector_ivf_build('bench', 'v', 'nlist=%d,nprobe=%d,max_memory=100MB');",
                 BENCH_NLIST, BENCH_NPROBE);
        printf("Building IVF index (nlist=%d) ...\n", BENCH_NLIST);
        t0 = now_ms();
        exec_sql(db, sql);
        double t_build = now_ms() - t0;
        printf("  IVF build:    %.1f ms\n", t_build);
    }

    /* ---- IVF preload ---- */
    t0 = now_ms();
    exec_sql(db, "SELECT vector_ivf_preload('bench', 'v');");
    double t_preload = now_ms() - t0;
    double mem_after_preload = get_rss_mb();
    printf("  IVF preload:  %.1f ms  RSS: %.1f MB\n", t_preload, mem_after_preload);

    /* ---- IVF search ---- */
    int64_t ivf_ids[BENCH_K];
    double t_ivf;
    double mem_before_ivf = get_rss_mb();
    int ivf_count = run_topk_search(db, "vector_ivf_scan", "bench",
                                    query, BENCH_DIM, BENCH_K,
                                    ivf_ids, &t_ivf);
    double mem_after_ivf = get_rss_mb();
    printf("  IVF search:   %.1f ms  (%d results)  RSS: %.1f MB\n", t_ivf, ivf_count, mem_after_ivf);

    /* ---- recall & speedup ---- */
    double recall  = compute_recall(bf_ids, bf_count, ivf_ids, ivf_count);
    double speedup = (t_ivf > 0.0) ? t_brute / t_ivf : 0.0;

    printf("\n");
    printf("  Recall@%d:    %.2f  (%d/%d ground-truth hits)\n",
           BENCH_K, recall, (int)round(recall * bf_count), bf_count);
    printf("  Speedup:      %.1fx  (%.1f ms -> %.1f ms)\n",
           speedup, t_brute, t_ivf);
    printf("\n  Memory:\n");
    printf("    Brute force search: %.1f MB (RSS before: %.1f MB, after: %.1f MB)\n",
           mem_after_bf - mem_before_bf, mem_before_bf, mem_after_bf);
    printf("    IVF preload:        %.1f MB (RSS after preload: %.1f MB)\n",
           mem_after_preload - mem_after_bf, mem_after_preload);
    printf("    IVF search:         %.1f MB (RSS before: %.1f MB, after: %.1f MB)\n",
           mem_after_ivf - mem_before_ivf, mem_before_ivf, mem_after_ivf);

    /* ---- assertions for regression detection ---- */
    printf("\n");
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "brute force returns %d results", BENCH_K);
        ASSERT(bf_count == BENCH_K, msg);
    }
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "IVF returns %d results", BENCH_K);
        ASSERT(ivf_count == BENCH_K, msg);
    }
    ASSERT(recall >= 0.3, "recall@10 >= 0.3");
    ASSERT(speedup > 1.0, "IVF search faster than brute force");

    /* ---- cleanup ---- */
    exec_sql(db, "SELECT vector_ivf_cleanup('bench', 'v');");
    free(vec);
    free(query);
    sqlite3_close(db);

    /* ---- summary ---- */
    printf("\n========================================\n");
    printf("Benchmark: %d passed, %d failed\n", passes, failures);
    printf("========================================\n");

    return failures > 0 ? 1 : 0;
}
