# Figure 6: Kogge-Stone Parallel Carry Tree

```latex
\begin{tikzpicture}
  \foreach \i in {0,...,7} {
    \node (B\i) at (\i, 0) {$b_{\i}$};
  }
  
  % Jumps (Offset 1)
  \draw[->] (B0) to[bend left] (B1);
  \draw[->] (B1) to[bend left] (B2);
  
  % Jumps (Offset 2)
  \draw[->] (B0) to[bend right] (B2);
  \draw[->] (B1) to[bend right] (B3);
  
  \node at (3.5, -1) {$\log_2(N)$ Parallel Steps};
\end{tikzpicture}
```

### ASCII Fallback
```
Bit 0 ----> Bit 1 ----> Bit 2 ----> Bit 3
  |          ^          ^          ^
  \----------/----------/----------/  (Jumps 1, 2, 4, 8, 16, 32)
```
