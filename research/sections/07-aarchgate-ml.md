# 7. Demonstration I: AarchGate-ML

The first validation of the AarchGate primitive is in the domain of machine learning inference, specifically for **Gradient-Boosted Decision Trees (GBDTs).** Standard GBDT engines (e.g., XGBoost, LightGBM) process records by traversing trees node-by-node. On modern processors, this leads to significant performance degradation due to branch mispredictions as the data dictates the traversal path.

## 7.1 Flattening Trees into Boolean Logic

**AarchGate-ML** fundamentally changes the inference model by flattening decision trees into a series of parallel bitwise logic gates. Instead of a tree structure, every decision path is represented as a boolean mask. 

For a tree with $D$ levels, the result is a 64-bit mask representing which of the 64 rows fall into a specific leaf. The JIT compiler emits the logic to evaluate all split conditions across all 64 rows simultaneously. This transformation effectively converts a search problem into a logic evaluation problem, achieving **Zero Branch Mispredictions.**

```cpp
// Listing 7.1: Tree Path to Logic Synthesis
// Path: feature[0] > 10.5 AND feature[2] < 5.0
uint64_t path_mask = engine.execute("feature0 > 10.5") & engine.execute("feature2 < 5.0");
```

## 7.2 Popcount Weight Accumulation

Once the leaf masks are calculated for a forest of trees, AarchGate-ML must accumulate the floating-point leaf weights. Traditional engines do this sequentially per row. AarchGate-ML utilizes the **`__builtin_popcountll`** intrinsic, which maps directly to the ARM64 **`CNT`** vector instruction.

The final prediction $P$ for a block of 64 rows is calculated as:
$$P = \sum_{i=1}^{Trees} (\text{Popcount}(Mask_i \ \& \ \text{Selector}) \times Weight_i)$$
By using hardware popcount, AarchGate-ML can accumulate weights across 64 rows in a single pass, drastically reducing the "Serialization Tax" at the end of the inference pipeline.

## 7.3 Performance vs. Native XGBoost

We evaluated AarchGate-ML against the native C++ XGBoost engine on an Apple M3 platform using a 100-tree model and 10 Million test rows.

| Metric | Native XGBoost | AarchGate-ML | Speedup |
| :--- | :---: | :---: | :---: |
| Throughput (Rows/s) | 2.10 Million | **61.34 Million** | **29.2x** |
| p99 Latency (Row) | 476.50 ns | **16.30 ns** | **29.2x** |
| Branch Mispredictions | 4.2% | **0.0%** | **Perfect** |

The results demonstrate that by eliminating branches and utilizing bit-sliced logic, AarchGate-ML operates at a performance tier unreachable by traditional tree-traversal engines.

![Figure 7: Decision Tree to Bit-Sliced Boolean Circuit](../figures/tree_flattening.png)
