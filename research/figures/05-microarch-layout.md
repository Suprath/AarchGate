# Figure 5: ARM64 P-Core Execution Width and Cache Paths

```latex
\begin{tikzpicture}
  \draw[fill=gray!20] (0,0) rectangle (6,4);
  \node at (3,3.5) {Apple M3 P-Core};
  
  \draw[fill=blue!10] (0.5, 0.5) rectangle (2.5, 1.5);
  \node at (1.5, 1) {8-wide Issue};
  
  \draw[fill=red!10] (3.5, 0.5) rectangle (5.5, 1.5);
  \node at (4.5, 1) {128KB L1D};
  
  \draw[<->, thick] (2.5, 1) -- (3.5, 1);
  \node at (3, 0.7) {Load/Store};
\end{tikzpicture}
```

### ASCII Fallback
```
+--------------------------+
|      Apple M3 P-Core     |
|  [ 8-wide Execution ] <-----> [ 128KB L1D Cache ]
|          (ILP=4.5)       |     (Cache-Line=64B)
+--------------------------+
```
