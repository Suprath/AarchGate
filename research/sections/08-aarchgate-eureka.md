# 8. Demonstration II: AarchGate-Eureka

The second validation domain is analytical query processing for schemaless NDJSON log data. Traditional log engines (e.g., Elasticsearch, Splunk) struggle with the "Parsing Tax"—the continuous overhead of string manipulation and JSON decoding during every query. **AarchGate-Eureka** solves this by pre-transposing logs into columnar bit-planes and using a deferred retrieval strategy.

## 8.1 The Parsing Tax and .agb Format

In standard log processing, the CPU spends up to 80% of its time executing string-to-numeric conversions and pointer chasing through JSON objects. Eureka eliminates this by converting raw NDJSON into the **AarchGate Binary (.agb)** format. In this format, numeric and categorical fields are stored as page-aligned bit-planes, ready for immediate ingestion by the AarchGate core engine.

## 8.2 Two-Pass Deferred Materialization

To maintain the flexibility of full-text log search while achieving the speed of columnar bit-slicing, Eureka implements a **Two-Pass execution model**:

*   **Pass 1: Logical JIT Scan**: The engine sweeps the compact `.agb` bit-planes at physical memory-bus speeds (61 GB/s). This pass returns a 64-bit result mask for every block, identifying which rows match the query criteria.
*   **Pass 2: Deferred Seek & Load**: For the small subset of rows that actually match (typically < 0.1% of logs), Eureka uses a companion `.agb.idx` file containing byte offsets. It performs a direct seek and read from the original raw JSON file to materialize the full human-readable log.

```cpp
// Listing 8.1: Eureka Two-Pass Logic
// Pass 1: Columnar bit-slice scan
uint64_t mask = aarchgate_engine_execute(query_id, bit_plane_buffer);

// Pass 2: Deferred raw retrieval
if (mask != 0) {
    for (int row = 0; row < 64; ++row) {
        if ((mask >> row) & 1) {
            size_t offset = idx_offsets[block_base + row];
            materialize_raw_log(offset); // Minimal I/O
        }
    }
}
```

## 8.3 Performance Evaluation: Eureka vs. Elasticsearch

We evaluated AarchGate-Eureka against Elasticsearch (v8.x) on a 100 GB synthetic Apache Access Log dataset.

| Metric | Elasticsearch | AarchGate-Eureka | Speedup |
| :--- | :---: | :---: | :---: |
| Scan Throughput | ~8 GB/s | **61 GB/s** | **7.6x** |
| Query Latency (1M logs) | 30.4 ms | **0.03 ms** | **1,000x** |
| Ingestion Rate | 300K events/s | **10M events/s** | **33.3x** |

**Reconstruction Verification:** For a query returning 1,000 records from a 1,000,000 record set, Eureka completes the entire scan and raw retrieval process in **181 microseconds.** This confirms that the two-pass approach effectively hides the I/O cost of raw log retrieval behind the speed of the bit-sliced logical scan.

![Figure 8: Two-Pass Deferred Materialization Flow](../figures/eureka_materialization.png)
