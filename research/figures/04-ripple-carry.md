# Figure 4: Ripple-Carry Logic Transition Diagram

```latex
\begin{tikzpicture}[node distance=2.5cm]
  \node (start) [circle, draw] {Bit $i$};
  \node (cond0) [rectangle, draw, right of=start] {$K_i = 0?$};
  \node (step0) [rectangle, draw, below of=cond0] {$G = G \cup (E \cap A_i)$ \\ $E = E \cap \neg A_i$};
  \node (step1) [rectangle, draw, right of=cond0] {$E = E \cap A_i$};
  
  \draw [->] (start) -- (cond0);
  \draw [->] (cond0) -- node[left] {Yes} (step0);
  \draw [->] (cond0) -- node[above] {No} (step1);
\end{tikzpicture}
```

### ASCII Fallback
```
       [ Bit i ]
           |
    /------^------\
  Ki=0?         Ki=1?
    |             |
    v             v
[GT Update]   [EQ Update]
[EQ Update]
```
