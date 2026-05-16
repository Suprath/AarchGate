# Figure 7: Decision Tree to Bit-Sliced Boolean Circuit

```latex
\begin{tikzpicture}
  % Tree
  \node {Root}
    child {node {L1} child {node {Leaf 1}} child {node {Leaf 2}}}
    child {node {L2} child {node {Leaf 3}} child {node {Leaf 4}}};
    
  \node at (5, -1) {$\Longrightarrow$};
  
  % Logic
  \node at (8, -1) {Mask = ($f_0 > T_0$) $\cap$ ($f_1 < T_1$)};
\end{tikzpicture}
```

### ASCII Fallback
```
   [ Root ]                    [ Parallel Logic ]
   /      \           ===>     Mask = (A > T1) & (B < T2)
[ L ]    [ R ]                 (Zero Branching)
```
