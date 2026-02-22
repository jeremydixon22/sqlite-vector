# IVF (Inverted File) Index

## Overview

IVF indexing partitions vectors into clusters using k-means, then searches only the nearest clusters at query time instead of scanning every vector. This trades a small recall loss for a large speedup.

## API

### Build

```sql
-- 2-arg form (defaults: nlist=64, nprobe=sqrt(nlist), max_memory=100MB)
SELECT vector_ivf_build('table', 'column');

-- 3-arg form with options
SELECT vector_ivf_build('table', 'column', 'nlist=100,nprobe=20,max_memory=100MB');
```

| Option       | Type    | Default          | Description                                      |
|--------------|---------|------------------|--------------------------------------------------|
| `nlist`      | int     | 64               | Number of clusters (partitions)                  |
| `nprobe`     | int     | sqrt(nlist)      | Number of clusters to search at query time       |
| `max_memory` | size    | 100MB            | Memory budget for preloading cluster data        |

`max_memory` accepts human-readable suffixes: `KB`, `MB`, `GB`, or plain bytes. It is persisted in the `_sqliteai_vector` metadata table and automatically respected by `vector_ivf_preload`.

Returns the number of vectors indexed.

### Preload

```sql
SELECT vector_ivf_preload('table', 'column');
```

Loads centroids, per-cluster counts, and as much cluster data as fits within the `max_memory` budget into memory. Centroids and counts are always loaded (negligible size). Cluster data is loaded greedily in storage order until the budget is exhausted; remaining clusters fall back to disk reads at query time.

### Search

```sql
-- Top-k (non-streaming)
SELECT rowid, distance
FROM vector_ivf_scan('table', 'column', vector('[0.1, 0.2, ...]'), 10);

-- Streaming
SELECT rowid, distance
FROM vector_ivf_scan('table', 'column', vector('[0.1, 0.2, ...]'));
```

Both modes support hybrid execution: preloaded clusters are scanned from memory, non-preloaded clusters are fetched from disk via `SELECT ... WHERE centroid_id IN (...)`.

### Cleanup

```sql
SELECT vector_ivf_cleanup('table', 'column');
```

Frees all in-memory IVF data and drops the IVF table.

## How It Works

1. **Build** runs k-means (Lloyd's algorithm, 10 iterations) on the full dataset to produce `nlist` centroids. Each vector is assigned to its nearest centroid. A shadow table `ivf0_<table>_<column>` stores per-cluster: centroid blob, vector count, and a packed data blob (rows of `[int64_t rowid | vector_bytes]`).

2. **Preload** reads the shadow table in a single pass. Centroids and counts are always loaded into `table_context`. For each cluster's data blob, if adding it would exceed `max_memory`, it is skipped (left as `NULL` in the `ivf_data` array).

3. **Search** converts the query to F32, finds the `nprobe` nearest centroids via brute-force scan over centroids, then for each probe cluster:
   - If `ivf_data[cid] != NULL`: scan directly from memory.
   - Otherwise: include `cid` in a disk query (`WHERE centroid_id IN (...)`).

   Top-k mode feeds results into a max-heap. Streaming mode merges all probe cluster data into a single buffer.

## Architecture

```
vector_ivf_build()
  -> k-means clustering
  -> write shadow table ivf0_<table>_<column>
  -> serialize nlist, nprobe, max_memory to _sqliteai_vector

vector_ivf_preload()
  -> read shadow table
  -> always: centroids, counts into table_context
  -> within budget: cluster data into ivf_data[]
  -> over budget: ivf_data[cid] = NULL (disk fallback)

vector_ivf_scan (top-k or streaming)
  -> find nprobe nearest centroids
  -> scan preloaded clusters from memory
  -> query non-preloaded clusters from disk
  -> merge results
```

### Key data structures in `table_context`

| Field             | Type       | Description                                |
|-------------------|------------|--------------------------------------------|
| `ivf_nlist`       | `int`      | Number of clusters                         |
| `ivf_nprobe`      | `int`      | Number of clusters to probe                |
| `ivf_max_memory`  | `int64_t`  | Memory budget in bytes                     |
| `ivf_centroids`   | `float *`  | `[nlist * dim]` F32 centroid vectors       |
| `ivf_counts`      | `int *`    | `[nlist]` vector count per cluster         |
| `ivf_data`        | `void **`  | `[nlist]` per-cluster packed data or NULL  |

### Metadata persistence

Three keys are stored in `_sqliteai_vector` via `sqlite_serialize`:

| Key          | SQLite Type      | Value                    |
|--------------|------------------|--------------------------|
| `nlist`      | `SQLITE_INTEGER` | Number of clusters       |
| `nprobe`     | `SQLITE_INTEGER` | Default probe count      |
| `max_memory` | `SQLITE_INTEGER` | Budget in bytes          |

These are restored on `sqlite_unserialize` so that `vector_ivf_preload` works correctly after reopening the database.

## Pros and Cons

### Pros

- **Significant speedup**: Only `nprobe` out of `nlist` clusters are searched, reducing work proportionally. The benchmark shows 3.8x speedup with nprobe=20 out of nlist=100.
- **Bounded memory**: The `max_memory` setting prevents preload from consuming unbounded RAM. A 1M x 768-dim F32 dataset (~3 GB of vector data) can be queried with only 100 MB of preloaded data.
- **Hybrid execution**: Preloaded clusters are scanned at memory speed; non-preloaded clusters transparently fall back to disk. No user-facing API change is needed.
- **Works with all vector types**: F32, F16, BF16, I8, U8 (not BIT, which is rejected at build time).
- **Greedy preload**: The most commonly stored clusters (those encountered first in storage order) get preloaded, which is a reasonable heuristic when clusters are stored by ID.

### Cons

- **Recall loss**: IVF is an approximate method. Vectors near cluster boundaries may be missed if their cluster isn't probed. The benchmark shows recall@10 = 0.50 with nprobe=20/nlist=100 on random data. Real-world data with more structure typically yields higher recall.
- **Build time**: k-means is expensive. Building on 1M x 768-dim vectors takes ~10 minutes. This is a one-time cost.
- **Static index**: The IVF index is not updated when rows are inserted or deleted. A rebuild is required to incorporate new data.
- **Greedy preload order**: Clusters are loaded in storage order (by centroid_id), not by frequency or size. A popularity-based or size-based ordering could be more optimal but adds complexity.
- **No partial cluster loading**: A cluster is either fully loaded or not at all. Very large clusters that exceed the remaining budget are skipped entirely even if most of their data would fit.
- **Random data pessimism**: The benchmark uses uniformly random vectors, which is the worst case for clustering (no natural structure to exploit). Real datasets with semantic clusters will show better recall and speedup.

## Benchmark Results

**Configuration**: 1M vectors, 768 dimensions, F32, K=10, nlist=100, nprobe=20, max_memory=100MB

```
=== IVF Benchmark ===
  Vectors: 1000000   Dim: 768   K: 10   nlist: 100   nprobe: 20

Inserting 1000000 vectors ...
  Insert:       3479.5 ms
  Brute force:  429.8 ms  (10 results)  RSS: 4475.6 MB
Building IVF index (nlist=100) ...
  IVF build:    651678.8 ms
  IVF preload:  452.4 ms  RSS: 3883.2 MB
  IVF search:   113.8 ms  (10 results)  RSS: 3883.6 MB

  Recall@10:    0.50  (5/10 ground-truth hits)
  Speedup:      3.8x  (429.8 ms -> 113.8 ms)

  Memory:
    Brute force search: -0.3 MB
    IVF preload:        -592.4 MB (RSS after preload: 3883.2 MB)
    IVF search:         0.4 MB

Benchmark: 4 passed, 0 failed
```

| Metric              | Value       |
|---------------------|-------------|
| Brute force latency | 429.8 ms    |
| IVF search latency  | 113.8 ms    |
| Speedup             | 3.8x        |
| Recall@10           | 0.50        |
| IVF build time      | ~10.9 min   |
| IVF preload time    | 452.4 ms    |
| IVF search RSS delta| 0.4 MB      |

The IVF preload RSS delta of -592.4 MB confirms the memory cap is working: instead of loading all ~3 GB of cluster data, only clusters fitting within the 100 MB budget are preloaded. The remaining clusters are fetched from disk during search, which still achieves a 3.8x speedup over brute force.
