# Figure 8: Two-Pass Deferred Materialization Flow

```latex
\begin{tikzpicture}[node distance=2cm]
  \node (pass1) [rectangle, draw, fill=blue!10] {Pass 1: Columnar Scan (.agb)};
  \node (mask) [circle, draw, below of=pass1] {Mask};
  \node (pass2) [rectangle, draw, fill=green!10, right of=mask, xshift=3cm] {Pass 2: Seek \& Load (.json)};
  
  \draw [->] (pass1) -- (mask);
  \draw [->] (mask) -- node[above] {if bit=1} (pass2);
\end{tikzpicture}
```

### ASCII Fallback
```
[ Pass 1: Scan Bit-Planes (61 GB/s) ]
               |
               v
        [ Result Mask ] --(if bit=1)--> [ Pass 2: Seek original JSON ]
```
