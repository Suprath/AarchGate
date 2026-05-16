# Figure 3: 6-Stage Butterfly Network Swaps

```latex
\begin{tikzpicture}
  \foreach \i in {0,...,7} {
    \node (L\i) at (0, -\i) {Row \i};
    \node (R\i) at (4, -\i) {};
  }
  
  % Stage 5 (Stride 4 for this 8-row example)
  \draw[red, thick] (L0) -- (4, -4);
  \draw[red, thick] (L4) -- (4, 0);
  \draw[red, thick] (L1) -- (4, -5);
  \draw[red, thick] (L5) -- (4, -1);
  
  \node at (2, -8) {Recursive bit-swaps (Stride $2^k$)};
\end{tikzpicture}
```

### ASCII Fallback
```
Row 0 --\ /-- Row 4 (Stride 32)
Row 1 --/ \-- Row 5
...
[ 6 Stages total: 32, 16, 8, 4, 2, 1 ]
```
