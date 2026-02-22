/*
 * test_vector.c
 * Comprehensive unit tests for the SQLite Vector extension.
 *
 * Compiled with -DSQLITE_CORE so sqlite3_vector_init links statically.
 * Usage: gcc -DSQLITE_CORE ... -o test_vector && ./test_vector
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sqlite3.h"
#include "sqlite-vector.h"

/* ---------- Test infrastructure ---------- */

static int failures = 0;
static int passes   = 0;

#define ASSERT(cond, msg) do {                        \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else         { printf("PASS: %s\n", msg); passes++;    } \
} while (0)

/* Execute SQL that must succeed; returns SQLITE_OK or aborts the test. */
static int exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("  SQL error (%d): %s\n  Statement: %s\n", rc, err ? err : "unknown", sql);
        sqlite3_free(err);
    }
    return rc;
}

/* ---------- Helper: create, populate, and init a vector table ---------- */

/*
 * Sets up a table named `tbl` with columns (id INTEGER PRIMARY KEY, v BLOB),
 * inserts `n` vectors of the given type converted from JSON via vector_as_<type>(),
 * and calls vector_init() with the specified type, distance, and dimension.
 *
 * `vecs` is an array of JSON strings, e.g. "[1.0, 2.0, 3.0]".
 */
static int setup_table(sqlite3 *db, const char *tbl, const char *type,
                       const char *distance, int dim,
                       const char **vecs, int n) {
    char sql[2048];

    /* Create table */
    snprintf(sql, sizeof(sql), "CREATE TABLE \"%s\" (id INTEGER PRIMARY KEY, v BLOB);", tbl);
    if (exec_sql(db, sql) != SQLITE_OK) return -1;

    /* Insert vectors */
    for (int i = 0; i < n; i++) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO \"%s\" (id, v) VALUES (%d, vector_as_%s('%s'));",
                 tbl, i + 1, type, vecs[i]);
        if (exec_sql(db, sql) != SQLITE_OK) return -1;
    }

    /* vector_init */
    snprintf(sql, sizeof(sql),
             "SELECT vector_init('%s', 'v', 'type=%s,dimension=%d,distance=%s');",
             tbl, type, dim, distance);
    if (exec_sql(db, sql) != SQLITE_OK) return -1;

    return 0;
}

/* ---------- Callback helpers for querying results ---------- */

typedef struct {
    int    count;
    double distances[64];
} scan_result;

static int scan_cb(void *ctx, int ncols, char **vals, char **names) {
    (void)names;
    scan_result *r = (scan_result *)ctx;
    if (r->count < 64 && ncols >= 2 && vals[1]) {
        r->distances[r->count] = atof(vals[1]);
    }
    r->count++;
    return 0;
}

/* Like scan_cb but reads distance from column 0 (for queries where distance is first). */
static int scan_cb_col0(void *ctx, int ncols, char **vals, char **names) {
    (void)names;
    scan_result *r = (scan_result *)ctx;
    if (r->count < 64 && ncols >= 1 && vals[0]) {
        r->distances[r->count] = atof(vals[0]);
    }
    r->count++;
    return 0;
}

/* ---------- Test: basics ---------- */

static void test_basics(sqlite3 *db) {
    printf("\n=== Basics ===\n");

    /* vector_version() */
    {
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(db, "SELECT vector_version();", -1, &stmt, NULL);
        ASSERT(rc == SQLITE_OK, "vector_version() prepares");
        if (rc == SQLITE_OK) {
            rc = sqlite3_step(stmt);
            ASSERT(rc == SQLITE_ROW, "vector_version() returns a row");
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            ASSERT(v != NULL && strlen(v) > 0, "vector_version() returns non-empty text");
        }
        sqlite3_finalize(stmt);
    }

    /* vector_backend() */
    {
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(db, "SELECT vector_backend();", -1, &stmt, NULL);
        ASSERT(rc == SQLITE_OK, "vector_backend() prepares");
        if (rc == SQLITE_OK) {
            rc = sqlite3_step(stmt);
            ASSERT(rc == SQLITE_ROW, "vector_backend() returns a row");
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            ASSERT(v != NULL && strlen(v) > 0, "vector_backend() returns non-empty text");
        }
        sqlite3_finalize(stmt);
    }
}

/* ---------- Test: vector_full_scan for a given (type, distance) pair ---------- */

static void test_full_scan(sqlite3 *db, const char *type, const char *distance,
                           int dim, const char **vecs, int nvecs,
                           const char *query_vec) {
    char tbl[64], sql[1024], msg[256];
    snprintf(tbl, sizeof(tbl), "tfs_%s_%s", type, distance);

    /* lowercase table name for uniqueness */
    for (char *p = tbl; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;

    if (setup_table(db, tbl, type, distance, dim, vecs, nvecs) != 0) {
        snprintf(msg, sizeof(msg), "full_scan setup %s/%s", type, distance);
        ASSERT(0, msg);
        return;
    }

    /* DOT distance returns negative dot product, so skip non-negative checks */
    int is_dot = (strcasecmp(distance, "DOT") == 0);

    /* Top-k mode (k=3) */
    {
        scan_result r = {0};
        snprintf(sql, sizeof(sql),
                 "SELECT id, distance FROM vector_full_scan('%s', 'v', vector_as_%s('%s'), 3);",
                 tbl, type, query_vec);
        char *err = NULL;
        int rc = sqlite3_exec(db, sql, scan_cb, &r, &err);
        snprintf(msg, sizeof(msg), "full_scan top-k executes (%s/%s)", type, distance);
        ASSERT(rc == SQLITE_OK, msg);
        if (err) { printf("  err: %s\n", err); sqlite3_free(err); }

        snprintf(msg, sizeof(msg), "full_scan top-k returns 3 rows (%s/%s)", type, distance);
        ASSERT(r.count == 3, msg);

        /* Distances should be non-negative (DOT returns negative dot product, so skip) */
        if (!is_dot) {
            int all_non_neg = 1;
            for (int i = 0; i < r.count; i++) {
                if (r.distances[i] < 0) all_non_neg = 0;
            }
            snprintf(msg, sizeof(msg), "full_scan top-k distances >= 0 (%s/%s)", type, distance);
            ASSERT(all_non_neg, msg);
        }

        /* Distances should be sorted ascending */
        int sorted = 1;
        for (int i = 1; i < r.count; i++) {
            if (r.distances[i] < r.distances[i - 1]) sorted = 0;
        }
        snprintf(msg, sizeof(msg), "full_scan top-k distances sorted (%s/%s)", type, distance);
        ASSERT(sorted, msg);
    }

    /* Streaming mode (no k, use LIMIT) */
    {
        scan_result r = {0};
        snprintf(sql, sizeof(sql),
                 "SELECT id, distance FROM vector_full_scan('%s', 'v', vector_as_%s('%s')) LIMIT 5;",
                 tbl, type, query_vec);
        char *err = NULL;
        int rc = sqlite3_exec(db, sql, scan_cb, &r, &err);
        snprintf(msg, sizeof(msg), "full_scan stream executes (%s/%s)", type, distance);
        ASSERT(rc == SQLITE_OK, msg);
        if (err) { printf("  err: %s\n", err); sqlite3_free(err); }

        snprintf(msg, sizeof(msg), "full_scan stream returns rows (%s/%s)", type, distance);
        ASSERT(r.count > 0, msg);

        if (!is_dot) {
            int all_non_neg = 1;
            for (int i = 0; i < r.count; i++) {
                if (r.distances[i] < 0) all_non_neg = 0;
            }
            snprintf(msg, sizeof(msg), "full_scan stream distances >= 0 (%s/%s)", type, distance);
            ASSERT(all_non_neg, msg);
        }
    }
}

/* ---------- Test: vector_quantize_scan for a given (type, qtype) pair ---------- */

static void test_quantize_scan(sqlite3 *db, const char *type, const char *qtype,
                               int dim, const char **vecs, int nvecs,
                               const char *query_vec) {
    char tbl[64], sql[1024], msg[256];
    snprintf(tbl, sizeof(tbl), "tqs_%s_%s", type, qtype);

    for (char *p = tbl; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;

    /* Use L2 distance (or HAMMING for BIT) */
    const char *distance = (strcasecmp(type, "BIT") == 0) ? "HAMMING" : "L2";

    if (setup_table(db, tbl, type, distance, dim, vecs, nvecs) != 0) {
        snprintf(msg, sizeof(msg), "quantize_scan setup %s/%s", type, qtype);
        ASSERT(0, msg);
        return;
    }

    /* vector_quantize */
    snprintf(sql, sizeof(sql),
             "SELECT vector_quantize('%s', 'v', 'qtype=%s');", tbl, qtype);
    if (exec_sql(db, sql) != SQLITE_OK) {
        snprintf(msg, sizeof(msg), "vector_quantize %s/%s", type, qtype);
        ASSERT(0, msg);
        return;
    }

    /* Top-k mode */
    {
        scan_result r = {0};
        snprintf(sql, sizeof(sql),
                 "SELECT id, distance FROM vector_quantize_scan('%s', 'v', vector_as_%s('%s'), 3);",
                 tbl, type, query_vec);
        char *err = NULL;
        int rc = sqlite3_exec(db, sql, scan_cb, &r, &err);
        snprintf(msg, sizeof(msg), "quantize_scan top-k executes (%s/%s)", type, qtype);
        ASSERT(rc == SQLITE_OK, msg);
        if (err) { printf("  err: %s\n", err); sqlite3_free(err); }

        snprintf(msg, sizeof(msg), "quantize_scan top-k returns rows (%s/%s)", type, qtype);
        ASSERT(r.count > 0, msg);
    }

    /* Streaming mode */
    {
        scan_result r = {0};
        snprintf(sql, sizeof(sql),
                 "SELECT id, distance FROM vector_quantize_scan('%s', 'v', vector_as_%s('%s')) LIMIT 5;",
                 tbl, type, query_vec);
        char *err = NULL;
        int rc = sqlite3_exec(db, sql, scan_cb, &r, &err);
        snprintf(msg, sizeof(msg), "quantize_scan stream executes (%s/%s)", type, qtype);
        ASSERT(rc == SQLITE_OK, msg);
        if (err) { printf("  err: %s\n", err); sqlite3_free(err); }

        snprintf(msg, sizeof(msg), "quantize_scan stream returns rows (%s/%s)", type, qtype);
        ASSERT(r.count > 0, msg);
    }
}

/* ---------- Test: vector_ivf_scan for a given (type, distance) pair ---------- */

static void test_ivf_scan(sqlite3 *db, const char *type, const char *distance,
                          int dim, const char **vecs, int nvecs,
                          const char *query_vec) {
    char tbl[64], sql[1024], msg[256];
    snprintf(tbl, sizeof(tbl), "tivf_%s_%s", type, distance);

    for (char *p = tbl; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;

    if (setup_table(db, tbl, type, distance, dim, vecs, nvecs) != 0) {
        snprintf(msg, sizeof(msg), "ivf_scan setup %s/%s", type, distance);
        ASSERT(0, msg);
        return;
    }

    /* vector_ivf_build with small nlist for 10 vectors */
    snprintf(sql, sizeof(sql),
             "SELECT vector_ivf_build('%s', 'v', 'nlist=3');", tbl);
    if (exec_sql(db, sql) != SQLITE_OK) {
        snprintf(msg, sizeof(msg), "vector_ivf_build %s/%s", type, distance);
        ASSERT(0, msg);
        return;
    }

    /* vector_ivf_preload */
    snprintf(sql, sizeof(sql),
             "SELECT vector_ivf_preload('%s', 'v');", tbl);
    if (exec_sql(db, sql) != SQLITE_OK) {
        snprintf(msg, sizeof(msg), "vector_ivf_preload %s/%s", type, distance);
        ASSERT(0, msg);
        return;
    }

    int is_dot = (strcasecmp(distance, "DOT") == 0);

    /* Top-k mode (k=3) */
    {
        scan_result r = {0};
        snprintf(sql, sizeof(sql),
                 "SELECT id, distance FROM vector_ivf_scan('%s', 'v', vector_as_%s('%s'), 3);",
                 tbl, type, query_vec);
        char *err = NULL;
        int rc = sqlite3_exec(db, sql, scan_cb, &r, &err);
        snprintf(msg, sizeof(msg), "ivf_scan top-k executes (%s/%s)", type, distance);
        ASSERT(rc == SQLITE_OK, msg);
        if (err) { printf("  err: %s\n", err); sqlite3_free(err); }

        snprintf(msg, sizeof(msg), "ivf_scan top-k returns 3 rows (%s/%s)", type, distance);
        ASSERT(r.count == 3, msg);

        if (!is_dot) {
            int all_non_neg = 1;
            for (int i = 0; i < r.count; i++) {
                if (r.distances[i] < 0) all_non_neg = 0;
            }
            snprintf(msg, sizeof(msg), "ivf_scan top-k distances >= 0 (%s/%s)", type, distance);
            ASSERT(all_non_neg, msg);
        }

        int sorted = 1;
        for (int i = 1; i < r.count; i++) {
            if (r.distances[i] < r.distances[i - 1]) sorted = 0;
        }
        snprintf(msg, sizeof(msg), "ivf_scan top-k distances sorted (%s/%s)", type, distance);
        ASSERT(sorted, msg);
    }

    /* Streaming mode */
    {
        scan_result r = {0};
        snprintf(sql, sizeof(sql),
                 "SELECT id, distance FROM vector_ivf_scan('%s', 'v', vector_as_%s('%s')) LIMIT 5;",
                 tbl, type, query_vec);
        char *err = NULL;
        int rc = sqlite3_exec(db, sql, scan_cb, &r, &err);
        snprintf(msg, sizeof(msg), "ivf_scan stream executes (%s/%s)", type, distance);
        ASSERT(rc == SQLITE_OK, msg);
        if (err) { printf("  err: %s\n", err); sqlite3_free(err); }

        snprintf(msg, sizeof(msg), "ivf_scan stream returns rows (%s/%s)", type, distance);
        ASSERT(r.count > 0, msg);
    }

    /* Cleanup */
    snprintf(sql, sizeof(sql),
             "SELECT vector_ivf_cleanup('%s', 'v');", tbl);
    exec_sql(db, sql);
}

/* ---------- Test vectors ---------- */

/* 4-dimensional float vectors for numeric types */
static const char *float_vecs[] = {
    "[1.0, 0.0, 0.0, 0.0]",
    "[0.0, 1.0, 0.0, 0.0]",
    "[0.0, 0.0, 1.0, 0.0]",
    "[0.0, 0.0, 0.0, 1.0]",
    "[1.0, 1.0, 0.0, 0.0]",
    "[0.0, 1.0, 1.0, 0.0]",
    "[0.0, 0.0, 1.0, 1.0]",
    "[1.0, 1.0, 1.0, 0.0]",
    "[0.0, 1.0, 1.0, 1.0]",
    "[1.0, 1.0, 1.0, 1.0]",
};
static const int float_nvecs = 10;
static const char *float_query = "[0.5, 0.5, 0.5, 0.5]";

/* Integer vectors (0-255 range for U8, -128..127 for I8) */
static const char *int_vecs[] = {
    "[10, 0, 0, 0]",
    "[0, 10, 0, 0]",
    "[0, 0, 10, 0]",
    "[0, 0, 0, 10]",
    "[10, 10, 0, 0]",
    "[0, 10, 10, 0]",
    "[0, 0, 10, 10]",
    "[10, 10, 10, 0]",
    "[0, 10, 10, 10]",
    "[10, 10, 10, 10]",
};
static const int int_nvecs = 10;
static const char *int_query = "[5, 5, 5, 5]";

/* 8-dimensional BIT vectors (0 or 1 values) */
static const char *bit_vecs[] = {
    "[1, 0, 0, 0, 0, 0, 0, 0]",
    "[0, 1, 0, 0, 0, 0, 0, 0]",
    "[0, 0, 1, 0, 0, 0, 0, 0]",
    "[0, 0, 0, 1, 0, 0, 0, 0]",
    "[1, 1, 0, 0, 0, 0, 0, 0]",
    "[0, 1, 1, 0, 0, 0, 0, 0]",
    "[0, 0, 1, 1, 0, 0, 0, 0]",
    "[1, 1, 1, 0, 0, 0, 0, 0]",
    "[0, 1, 1, 1, 0, 0, 0, 0]",
    "[1, 1, 1, 1, 0, 0, 0, 0]",
};
static const int bit_nvecs = 10;
static const char *bit_query = "[1, 0, 1, 0, 1, 0, 1, 0]";

/* ---------- Main ---------- */

int main(void) {
    sqlite3 *db;
    int rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        printf("FAIL: cannot open :memory: database\n");
        return 1;
    }

    /* Initialize the vector extension */
    char *errmsg = NULL;
    rc = sqlite3_vector_init(db, &errmsg, NULL);
    if (rc != SQLITE_OK) {
        printf("FAIL: sqlite3_vector_init returned %d: %s\n", rc, errmsg ? errmsg : "");
        sqlite3_close(db);
        return 1;
    }

    /* 1. Basics */
    test_basics(db);

    /* 2. vector_full_scan — float types × all distances */
    printf("\n=== vector_full_scan ===\n");
    {
        const char *float_types[] = {"f32", "f16", "bf16"};
        const char *distances[]   = {"L2", "SQUARED_L2", "COSINE", "DOT", "L1"};

        for (int t = 0; t < 3; t++) {
            for (int d = 0; d < 5; d++) {
                test_full_scan(db, float_types[t], distances[d],
                               4, float_vecs, float_nvecs, float_query);
            }
        }

        /* Integer types */
        const char *int_types[] = {"i8", "u8"};
        for (int t = 0; t < 2; t++) {
            for (int d = 0; d < 5; d++) {
                test_full_scan(db, int_types[t], distances[d],
                               4, int_vecs, int_nvecs, int_query);
            }
        }

        /* BIT — only HAMMING */
        test_full_scan(db, "bit", "HAMMING", 8, bit_vecs, bit_nvecs, bit_query);
    }

    /* 3. vector_quantize_scan — all vector types × quantization types */
    printf("\n=== vector_quantize_scan ===\n");
    {
        const char *qtypes[] = {"UINT8", "INT8", "1BIT"};

        /* Float types */
        const char *float_types[] = {"f32", "f16", "bf16"};
        for (int t = 0; t < 3; t++) {
            for (int q = 0; q < 3; q++) {
                test_quantize_scan(db, float_types[t], qtypes[q],
                                   4, float_vecs, float_nvecs, float_query);
            }
        }

        /* Integer types */
        const char *int_types[] = {"i8", "u8"};
        for (int t = 0; t < 2; t++) {
            for (int q = 0; q < 3; q++) {
                test_quantize_scan(db, int_types[t], qtypes[q],
                                   4, int_vecs, int_nvecs, int_query);
            }
        }

        /* BIT — quantize with 1BIT */
        test_quantize_scan(db, "bit", "1BIT", 8, bit_vecs, bit_nvecs, bit_query);
    }

    /* 4. Streaming ORDER BY (regression test for issue #43) */
    printf("\n=== Streaming ORDER BY ===\n");
    {
        /*
         * Regression test: vector_full_scan_stream with ORDER BY + LIMIT
         * must return results sorted by distance.  A bug in vFullScanBestIndex
         * caused orderByConsumed=1 even in streaming mode, so SQLite skipped
         * sorting and returned rows in table order.
         *
         * We insert vectors in REVERSE distance order (farthest first) so
         * that table-order != distance-order, exposing the bug.
         */
        const char *tbl = "tfs_stream_order";
        const char *vecs_ordered[] = {
            "[10.0, 0.0, 0.0, 0.0]",   /* id 1: far   */
            "[0.0, 10.0, 0.0, 0.0]",   /* id 2: far   */
            "[5.0, 5.0, 0.0, 0.0]",    /* id 3: far   */
            "[2.0, 0.0, 0.0, 0.0]",    /* id 4: mid   */
            "[1.0, 1.0, 0.0, 0.0]",    /* id 5: mid   */
            "[0.6, 0.6, 0.6, 0.6]",    /* id 6: close */
            "[0.4, 0.4, 0.4, 0.4]",    /* id 7: close */
            "[0.5, 0.5, 0.5, 0.5]",    /* id 8: exact */
            "[0.3, 0.3, 0.3, 0.3]",    /* id 9: close */
            "[0.7, 0.7, 0.7, 0.7]",    /* id 10: close */
        };
        const char *qvec = "[0.5, 0.5, 0.5, 0.5]";

        if (setup_table(db, tbl, "f32", "L2", 4, vecs_ordered, 10) == 0) {
            /* Helper callback: distance is column 0 in these queries */
            struct { int count; double distances[64]; } r;

            /* vector_full_scan_stream + JOIN + ORDER BY distance LIMIT */
            {
                memset(&r, 0, sizeof(r));
                char sql[1024];
                snprintf(sql, sizeof(sql),
                    "SELECT vss.distance, t.id"
                    " FROM \"%s\" t"
                    " INNER JOIN vector_full_scan_stream('%s', 'v', vector_as_f32('%s'))"
                    "   AS vss ON vss.rowid = t.id"
                    " ORDER BY vss.distance LIMIT 5;",
                    tbl, tbl, qvec);
                char *err = NULL;
                rc = sqlite3_exec(db, sql, scan_cb_col0, &r, &err);
                ASSERT(rc == SQLITE_OK, "stream+JOIN+ORDER BY executes");
                if (err) { printf("  err: %s\n", err); sqlite3_free(err); }

                ASSERT(r.count == 5, "stream+JOIN+ORDER BY returns 5 rows");

                int sorted = 1;
                for (int i = 1; i < r.count; i++) {
                    if (r.distances[i] < r.distances[i - 1]) sorted = 0;
                }
                ASSERT(sorted, "stream+JOIN+ORDER BY distances sorted");
                ASSERT(r.distances[0] < 0.01, "stream+JOIN+ORDER BY closest is ~0");
            }

            /* vector_full_scan (no k, streaming) + ORDER BY distance LIMIT */
            {
                memset(&r, 0, sizeof(r));
                char sql[1024];
                snprintf(sql, sizeof(sql),
                    "SELECT distance, rowid"
                    " FROM vector_full_scan_stream('%s', 'v', vector_as_f32('%s'))"
                    " ORDER BY distance LIMIT 5;",
                    tbl, qvec);
                char *err = NULL;
                rc = sqlite3_exec(db, sql, scan_cb_col0, &r, &err);
                ASSERT(rc == SQLITE_OK, "stream+ORDER BY executes");
                if (err) { printf("  err: %s\n", err); sqlite3_free(err); }

                ASSERT(r.count == 5, "stream+ORDER BY returns 5 rows");

                int sorted = 1;
                for (int i = 1; i < r.count; i++) {
                    if (r.distances[i] < r.distances[i - 1]) sorted = 0;
                }
                ASSERT(sorted, "stream+ORDER BY distances sorted");
                ASSERT(r.distances[0] < 0.01, "stream+ORDER BY closest is ~0");
            }
        }
    }

    /* 5. vector_ivf_scan — float types x L2, COSINE + integer types x L2 */
    printf("\n=== vector_ivf_scan ===\n");
    {
        const char *float_types[] = {"f32", "f16", "bf16"};
        const char *distances[] = {"L2", "COSINE"};
        for (int t = 0; t < 3; t++)
            for (int d = 0; d < 2; d++)
                test_ivf_scan(db, float_types[t], distances[d], 4, float_vecs, float_nvecs, float_query);

        /* Integer types x L2 */
        const char *int_types[] = {"i8", "u8"};
        for (int t = 0; t < 2; t++)
            test_ivf_scan(db, int_types[t], "L2", 4, int_vecs, int_nvecs, int_query);
    }

    /* 6. Backward-compat aliases */
    printf("\n=== Backward-compat aliases ===\n");
    {
        /* Set up a table for alias tests */
        const char *tbl = "tfs_alias";
        if (setup_table(db, tbl, "f32", "L2", 4, float_vecs, float_nvecs) == 0) {
            /* vector_full_scan_stream */
            {
                scan_result r = {0};
                char sql[512];
                snprintf(sql, sizeof(sql),
                         "SELECT id, distance FROM vector_full_scan_stream('%s', 'v', vector_as_f32('%s')) LIMIT 3;",
                         tbl, float_query);
                char *err = NULL;
                rc = sqlite3_exec(db, sql, scan_cb, &r, &err);
                ASSERT(rc == SQLITE_OK, "vector_full_scan_stream alias works");
                if (err) { printf("  err: %s\n", err); sqlite3_free(err); }
                ASSERT(r.count > 0, "vector_full_scan_stream returns rows");
            }

            /* vector_quantize_scan_stream */
            {
                char sql[512];
                snprintf(sql, sizeof(sql),
                         "SELECT vector_quantize('%s', 'v');", tbl);
                exec_sql(db, sql);

                scan_result r = {0};
                snprintf(sql, sizeof(sql),
                         "SELECT id, distance FROM vector_quantize_scan_stream('%s', 'v', vector_as_f32('%s')) LIMIT 3;",
                         tbl, float_query);
                char *err = NULL;
                rc = sqlite3_exec(db, sql, scan_cb, &r, &err);
                ASSERT(rc == SQLITE_OK, "vector_quantize_scan_stream alias works");
                if (err) { printf("  err: %s\n", err); sqlite3_free(err); }
                ASSERT(r.count > 0, "vector_quantize_scan_stream returns rows");
            }
        }
    }

    sqlite3_close(db);

    /* Summary */
    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", passes, failures);
    printf("========================================\n");

    return failures > 0 ? 1 : 0;
}
